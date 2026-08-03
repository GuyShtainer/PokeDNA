#include "rom_script.h"
#include <string.h>

/* ---------------------------------------------------------------- read window
 * The walker touches 1-8 bytes at a time at scattered addresses. On hardware each
 * of those would be an f_lseek+f_read (a whole SD sector) if done naively.
 * MEASURED over every NPC in Emerald + FireRed, one resolve touches a median of 2
 * and at most 6 distinct 512-byte regions, in <=4 contiguous runs — so two 512-byte
 * windows (one for the script, one for the text) turn ~40 reads into ~2-4. */
typedef struct { uint32_t base; uint32_t len; uint8_t buf[512]; } Win;

static bool win_get(const RomCtx* c, Win* w, uint32_t addr, uint32_t n, const uint8_t** p) {
  if (n > 32) return false;
  if (!(w->len && addr >= w->base && addr + n <= w->base + w->len)) {
    uint32_t base = addr & ~0x1FFu;
    uint32_t len  = 512;
    if (!rom_ptr_ok(c, base) || !rom_ptr_ok(c, base + len - 1)) {
      /* near the end of the file: shrink */
      len = 0;
      while (len < 512 && rom_ptr_ok(c, base + len)) len++;
      if (addr + n > base + len) return false;
    }
    if (!rom_read_at(c, base, w->buf, len)) return false;
    w->base = base; w->len = len;
    if (addr + n > base + len) return false;           /* straddles the window end */
  }
  *p = w->buf + (addr - w->base);
  return true;
}

/* Straddling reads (an instruction crossing a 512-byte boundary) fall back to a
 * direct read into a scratch of at most 32 bytes. */
static bool rd(const RomCtx* c, Win* w, uint32_t a, uint32_t n, uint8_t* tmp) {
  const uint8_t* p;
  if (win_get(c, w, a, n, &p)) { memcpy(tmp, p, n); return true; }
  return rom_read_at(c, a, tmp, n);
}
static uint8_t  rd8 (const RomCtx* c, Win* w, uint32_t a, bool* ok) {
  uint8_t t[1]; if (!rd(c, w, a, 1, t)) { *ok = false; return 0; } return t[0];
}
static uint16_t rd16(const RomCtx* c, Win* w, uint32_t a, bool* ok) {
  uint8_t t[2]; if (!rd(c, w, a, 2, t)) { *ok = false; return 0; }
  return (uint16_t)(t[0] | ((uint16_t)t[1] << 8));
}
static uint32_t rd32(const RomCtx* c, Win* w, uint32_t a, bool* ok) {
  uint8_t t[4]; if (!rd(c, w, a, 4, t)) { *ok = false; return 0; }
  return (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
}

/* ------------------------------------------------------------- opcode lengths
 * OPERAND bytes per opcode (the opcode byte itself is not counted). Mechanically
 * derived from pret/pokeemerald asm/macros/event.inc + data/script_cmd_table.inc,
 * where `map` = 2 bytes (group,num), `stringvar` = 1 byte, and
 * `formatwarp` = map(2) + warpId(1) + x(2) + y(2) = 7.
 * 0xFF = variable (trainerbattle), 0xFE = unknown/not a command.
 *
 * VERIFIED: handler names at 0x00..0xC6 are identical in pokeemerald, pokefirered
 * and pokeruby (only some renames), so ONE table covers all five games up to 0xC6;
 * only 0xC7.. diverges, and 0xC7.. never appears before a msgbox in practice. */
#define V 0xFF
#define X 0xFE
static const uint8_t k_len[256] = {
/*00*/ 0,0,0,0,4,4,5,5,1,1,2,2,0,0,1,5,
/*10*/ 2,5,5,5,2,8,4,4,4,4,4,2,2,5,5,5,
/*20*/ 8,4,4,4,4,2,4,0,2,2,2,2,4,0,0,2,
/*30*/ 0,2,0,3,2,0,2,1,1,7,7,7,2,7,7,7,
/*40*/ 7,7,4,0,4,4,4,4,2,4,4,2,2,2,2,6,
/*50*/ 8,2,4,2,4,2,4,6,4,4,0,3,V,0,0,0,
/*60*/ 2,2,2,6,2,3,0,4,0,0,0,0,0,0,2,4,
/*70*/ 5,5,0,4,4,4,0,1,4,14,2,4,2,3,1,2,
/*80*/ 3,3,3,3,3,5,4,4,4,2,3,0,0,0,0,2,
/*90*/ 5,5,5,3,2,3,2,1,2,2,1,4,2,3,2,2,
/*A0*/ 0,4,8,0,2,0,1,2,5,4,8,2,4,4,0,4,
/*B0*/ 4,7,0,2,2,2,5,0,4,4,4,5,5,4,4,5,
/*C0*/ 2,2,2,1,7,0,3,1,4,0,0,0,5,2,2,0,
/*D0*/ 2,7,3,2,0,2,0,7,0,0,0,4,1,3,3,4,
/*E0*/ 7,3,5,X,X,X,X,X,X,X,X,X,X,X,X,X,
/*F0*/ X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
};
/* trainerbattle: bytes AFTER type(1)+trainerId(2)+localId(2), indexed by type.
 * pokeemerald include/constants/battle_setup.h:4-16 + the trainerbattle macro. */
static const uint8_t k_tb[16] = { 8,12,12,4,12,8,16,12,16,8,8,8,8,X,X,X };

#define OP_END 0x02
#define OP_RET 0x03

static int cmd_len(const RomCtx* c, Win* w, uint32_t pc, uint8_t op) {
  if (op == 0x5C) {                       /* trainerbattle */
    bool ok = true;
    uint8_t t = rd8(c, w, pc + 1, &ok);
    if (!ok || t >= 16 || k_tb[t] == X) return -1;
    return 1 + 1 + 2 + 2 + k_tb[t];
  }
  if (k_len[op] == X) return -1;
  return 1 + k_len[op];
}

/* gStdScripts indices that end in a message box (asm/macros/event.inc MSGBOX_*):
 * 2 NPC, 3 SIGN, 4 DEFAULT, 5 YESNO, 6 AUTOCLOSE, 9 GETPOINTS, 10 POKENAV.
 * 0/1/7/8 also print, but from a *buffered* string, not the loadword pointer. */
static bool is_msgbox_std(uint8_t n) {
  return n == 2 || n == 3 || n == 4 || n == 5 || n == 6 || n == 9 || n == 10;
}

#define MAX_CMDS   24
#define MAX_ALTS   16
#define MAX_DEPTH   4

uint16_t rom_script_item_ball(const RomCtx* c, uint32_t s) {
  Win w = {0}; bool ok = true;
  if (rd8(c, &w, s, &ok) != 0x1A || !ok) return 0;
  if (rd16(c, &w, s + 1, &ok) != 0x8000 || !ok) return 0;
  uint16_t item = rd16(c, &w, s + 3, &ok);
  uint32_t p = s + 5;
  if (rd8(c, &w, p, &ok) == 0x1A && rd16(c, &w, p + 1, &ok) == 0x8001) p += 5;
  uint8_t op = rd8(c, &w, p, &ok);
  uint8_t n  = rd8(c, &w, p + 1, &ok);
  if (!ok || op != 0x09 || n > 1) return 0;
  return item;
}

bool rom_script_find_dialog(const RomCtx* c, uint32_t script, RomDialogRef* out) {
  if (!c || !out || !rom_ptr_ok(c, script)) return false;

  uint32_t alts[MAX_ALTS];
  uint8_t  nalts = 0, ai = 0;
  uint32_t seen[MAX_ALTS + 1];
  uint8_t  nseen = 0;
  bool     cond_seen = false;
  Win      w = {0};

  uint32_t entry = script;
  bool     is_alt = false;
  for (;;) {
    /* de-dup entry points */
    bool dup = false;
    for (uint8_t i = 0; i < nseen; i++) if (seen[i] == entry) dup = true;
    if (!dup && nseen < MAX_ALTS + 1) seen[nseen++] = entry;
    if (dup) goto next_entry;

    {
      uint32_t pc = entry, ret[MAX_DEPTH];
      uint8_t  depth = 0;
      uint32_t word0 = 0;                 /* last `loadword 0, X`                  */
      bool     path_cond = false;

      for (int step = 0; step < MAX_CMDS; step++) {
        bool ok = true;
        if (!rom_ptr_ok(c, pc)) break;
        uint8_t op = rd8(c, &w, pc, &ok);
        if (!ok) break;
        int ln = cmd_len(c, &w, pc, op);
        if (ln < 0) break;                /* unknown opcode: this path is not code */

        if (op == 0x0F) {                 /* loadword destIndex, value             */
          if (rd8(c, &w, pc + 1, &ok) == 0) word0 = rd32(c, &w, pc + 2, &ok);
        } else if (op == 0x09 || op == 0x08) {   /* callstd / gotostd              */
          uint8_t n = rd8(c, &w, pc + 1, &ok);
          if (ok && is_msgbox_std(n) && word0 && rom_ptr_ok(c, word0)) {
            out->text_addr = word0;
            out->via       = ROM_DLG_VIA_MSGBOX;
            out->certainty = is_alt ? ROM_DLG_BRANCH
                           : ((cond_seen || path_cond) ? ROM_DLG_FIRST_OF_N : ROM_DLG_CERTAIN);
            return true;
          }
          if (op == 0x08) break;          /* gotostd never comes back              */
        } else if (op == 0x67 || op == 0x9B || op == 0xDB || op == 0xBD || op == 0x78) {
          uint32_t t = rd32(c, &w, pc + 1, &ok);   /* message / autoscroll / instant
                                                    / vmessage / braillemessage    */
          if (ok && t && rom_ptr_ok(c, t)) {
            out->text_addr = t;
            out->via       = ROM_DLG_VIA_MESSAGE;
            out->certainty = is_alt ? ROM_DLG_BRANCH
                           : ((cond_seen || path_cond) ? ROM_DLG_FIRST_OF_N : ROM_DLG_CERTAIN);
            return true;
          }
        } else if (op == 0x5C) {          /* trainerbattle: pointer1 = intro text   */
          uint32_t t = rd32(c, &w, pc + 6, &ok);
          if (ok && t && rom_ptr_ok(c, t)) {
            out->text_addr = t;
            out->via       = ROM_DLG_VIA_TRAINER;
            out->certainty = is_alt ? ROM_DLG_BRANCH
                           : ((cond_seen || path_cond) ? ROM_DLG_FIRST_OF_N : ROM_DLG_CERTAIN);
            return true;
          }
        } else if (op == 0x05) {          /* goto                                   */
          uint32_t d = rd32(c, &w, pc + 1, &ok);
          if (!ok || !rom_ptr_ok(c, d)) break;
          pc = d;  continue;
        } else if (op == 0x04) {          /* call                                   */
          uint32_t d = rd32(c, &w, pc + 1, &ok);
          if (!ok || !rom_ptr_ok(c, d) || depth >= MAX_DEPTH) break;
          ret[depth++] = pc + (uint32_t)ln;
          pc = d;  continue;
        } else if (op == 0x06 || op == 0x07) {   /* goto_if / call_if              */
          uint32_t d = rd32(c, &w, pc + 2, &ok);
          path_cond = true;
          if (!is_alt) cond_seen = true;
          if (ok && rom_ptr_ok(c, d) && nalts < MAX_ALTS) alts[nalts++] = d;
        } else if (op == OP_END || op == 0x0C || op == 0x0D) {
          break;
        } else if (op == OP_RET) {
          if (depth) { pc = ret[--depth]; continue; }
          break;
        }
        pc += (uint32_t)ln;
      }
    }

  next_entry:
    if (ai >= nalts) return false;
    entry  = alts[ai++];
    is_alt = true;
  }
}

/* ----------------------------------------------------------------- text decode
 * English charmap only (pret charmap.txt lines 1..157). Past that the same byte
 * values are re-mapped to kana for the Japanese builds; using those silently turns
 * "POKeBLOCK" (55 56 57 58 59) into katakana.  Anything not listed -> invalid. */
static const char* k_glyph[256] = {
  [0x00] = " ",
  [0x01] = "A", [0x02] = "A", [0x03] = "A", [0x04] = "C", [0x05] = "E", [0x06] = "E",
  [0x07] = "E", [0x08] = "E", [0x09] = "I", [0x0B] = "I", [0x0C] = "I", [0x0D] = "O",
  [0x0E] = "O", [0x0F] = "O", [0x10] = "OE", [0x11] = "U", [0x12] = "U", [0x13] = "U",
  [0x14] = "N", [0x15] = "ss", [0x16] = "a", [0x17] = "a", [0x19] = "c", [0x1A] = "e",
  [0x1B] = "e", [0x1C] = "e", [0x1D] = "e", [0x1E] = "i", [0x20] = "i", [0x21] = "i",
  [0x22] = "o", [0x23] = "o", [0x24] = "o", [0x25] = "oe", [0x26] = "u", [0x27] = "u",
  [0x28] = "u", [0x29] = "n", [0x2A] = "o", [0x2B] = "a", [0x2C] = "er", [0x2D] = "&",
  [0x2E] = "+", [0x34] = "Lv", [0x35] = "=", [0x36] = ";",
  [0x51] = "?", [0x52] = "!", [0x53] = "PK",
  [0x5A] = "I", [0x5B] = "%", [0x5C] = "(", [0x5D] = ")",
  [0x68] = "a", [0x6F] = "i", [0x77] = " ", [0x79] = "^", [0x7A] = "v", [0x7B] = "<",
  [0x7C] = ">", [0x84] = "e", [0x85] = "<", [0x86] = ">", [0xA0] = "re",
  [0xA1] = "0", [0xA2] = "1", [0xA3] = "2", [0xA4] = "3", [0xA5] = "4", [0xA6] = "5",
  [0xA7] = "6", [0xA8] = "7", [0xA9] = "8", [0xAA] = "9",
  [0xAB] = "!", [0xAC] = "?", [0xAD] = ".", [0xAE] = "-", [0xAF] = "*", [0xB0] = "...",
  [0xB1] = "\"", [0xB2] = "\"", [0xB3] = "'", [0xB4] = "'", [0xB5] = "(M)", [0xB6] = "(F)",
  [0xB7] = "$", [0xB8] = ",", [0xB9] = "x", [0xBA] = "/",
  [0xEF] = "->", [0xF0] = ":", [0xF1] = "A", [0xF2] = "O", [0xF3] = "U",
  [0xF4] = "a", [0xF5] = "o", [0xF6] = "u",
};
/* 0xBB..0xD4 = 'A'..'Z', 0xD5..0xEE = 'a'..'z' handled arithmetically. */

/* 0xFC ext-control-code OPERAND lengths — pokeemerald src/text.c GetStringWidth(),
 * the EXT_CTRL_CODE_BEGIN switch. */
static const int8_t k_ext[0x19] = { -1,1,1,1,3,1,1,0,1,0,0,2,1,1,1,0,2,1,1,1,1,0,0,0,0 };

static const char* k_place(uint8_t v) {
  switch (v) {
    case 0x01: return "<PLAYER>";
    case 0x02: return "<VAR1>";
    case 0x03: return "<VAR2>";
    case 0x04: return "<VAR3>";
    case 0x06: return "<RIVAL>";
    case 0x07: return "<VERSION>";
    default:   return "<?>";
  }
}

static int put(char* dst, int cap, int n, const char* s) {
  while (*s) { if (n + 1 >= cap) return -1; dst[n++] = *s++; }
  return n;
}

int rom_text_decode(const RomCtx* c, uint32_t addr, char* dst, int cap, int* out_lines) {
  if (!c || !dst || cap < 2 || !rom_ptr_ok(c, addr)) return -1;
  Win w = {0};
  int n = 0, lines = 1;
  for (int i = 0; i < 1024; ) {
    bool ok = true;
    uint8_t b = rd8(c, &w, addr + i, &ok);
    if (!ok) return -1;
    i++;
    if (b == 0xFF) { dst[n] = 0; if (out_lines) *out_lines = lines; return n; }
    if (b == 0xFE || b == 0xFA || b == 0xFB) {          /* newline / page breaks    */
      if (n + 1 >= cap) return -1;
      dst[n++] = '\n'; lines++; continue;
    }
    if (b == 0xFC) {                                    /* ext control code         */
      uint8_t e = rd8(c, &w, addr + i, &ok); i++;
      if (!ok || e >= 0x19 || k_ext[e] < 0) return -1;
      i += k_ext[e];
      continue;
    }
    if (b == 0xFD) {                                    /* placeholder              */
      uint8_t v = rd8(c, &w, addr + i, &ok); i++;
      if (!ok) return -1;
      n = put(dst, cap, n, k_place(v));
      if (n < 0) return -1;
      continue;
    }
    if (b == 0xF7 || b == 0xF9) { i++; continue; }      /* dynamic / extra symbol   */
    if (b == 0xF8) {                                    /* keypad icon              */
      uint8_t k = rd8(c, &w, addr + i, &ok); i++;
      if (!ok) return -1;
      n = put(dst, cap, n, k == 0 ? "A" : k == 1 ? "B" : k == 2 ? "L" : k == 3 ? "R" : "#");
      if (n < 0) return -1;
      continue;
    }
    if (b == 0x53) {                                    /* PK / PKMN ligature       */
      uint8_t nx = rd8(c, &w, addr + i, &ok);
      if (ok && nx == 0x54) { i++; n = put(dst, cap, n, "POKeMON"); }
      else                    n = put(dst, cap, n, "PK");
      if (n < 0) return -1;
      continue;
    }
    if (b == 0x55) {                                    /* POKEBLOCK ligature       */
      uint8_t t[4];
      if (rd(c, &w, addr + i, 4, t) && t[0] == 0x56 && t[1] == 0x57 && t[2] == 0x58 && t[3] == 0x59) {
        i += 4;
        n = put(dst, cap, n, "POKeBLOCK");
        if (n < 0) return -1;
        continue;
      }
      return -1;
    }
    if (b >= 0xBB && b <= 0xD4) { if (n + 1 >= cap) return -1; dst[n++] = (char)('A' + b - 0xBB); continue; }
    if (b >= 0xD5 && b <= 0xEE) { if (n + 1 >= cap) return -1; dst[n++] = (char)('a' + b - 0xD5); continue; }
    if (!k_glyph[b]) return -1;                          /* not a string            */
    n = put(dst, cap, n, k_glyph[b]);
    if (n < 0) return -1;
  }
  return -1;                                             /* no terminator in 1 KiB  */
}
