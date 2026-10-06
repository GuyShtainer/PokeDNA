#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_romhint_cfg_test.py -- lane romhint fix2 (A8): a REAL host round trip of config.cfg's code.

Dual-compiles the CFG block of source/pdna_main.c (from `#define CFG_PATH` up to sort_label) on the PC with FatFs, the savefile
verified-write, rumble and the app globals stubbed over an in-memory file, then asserts:
  1. the first-launch welcome's cfg_save_ex("romask","1") adds EXACTLY the line `romask=1` and nothing else;
  2. a config holding EVERY key (non-default values, incl. rstr/rdur and sprite-era lines) is byte-identical after cfg_load + cfg_save;
  3. an answered card stays answered across an unrelated cfg_save;
  4. romask=0 / empty / no value / malformed / an absent file all read UNANSWERED (and never get written back as answered).
It then recompiles MUTANTS of the extracted block and requires each to FAIL the harness (M3 inverted rom_welcome_asked, M4 the old-key harvest
forced false, M9 the pre-fix cfg_load that ignored rstr/rdur). A mutant that still passes is a defect. Needs a C compiler (`cc`); skips if none.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"

PRE = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include "sprite_era.h"
#define PATH_MAX 256
#define GB_ROM_PATH_MAX 128
#define PDNA_DIR "/PokeDNA"
#define APP_ROM_SLOTS 5
#define HIST_CAP_SHIFT 24u
#define sniprintf snprintf
#define siprintf sprintf
typedef enum { PK_RS = 0, PK_EMERALD, PK_FRLG } PkGame;
enum { PDNA_GEN1 = 1, PDNA_GEN2 = 2 };
enum { ANIM_BOX, ANIM_PARTY, ANIM_DEX, ANIM_DAYCARE, ANIM_SUMMARY, ANIM_COUNT };
typedef enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_DATE = 2 } BrSortKey;
typedef unsigned UINT;
typedef enum { FR_OK = 0, FR_NO_FILE = 4 } FRESULT;
typedef struct { int pos; } FIL;
typedef struct { int x; } DIR;
#define FA_READ 1
typedef enum { SF_OK = 0, SF_FAIL } SfStatus;

static char mem_file[4096]; static int mem_len = 0; static bool mem_exists = false;
static BrSortKey g_sort = SORT_NAME; static bool g_sortrev, g_show_all, g_show_hidden, g_yard_visitors, g_rom_art_off;
static int g_backup_mode, g_pc_last_box, gb_scale_mode;
static unsigned g_anim_mask = 0x0Bu;
static char g_cwd[PATH_MAX] = "/";
static char g_rom_path[APP_ROM_SLOTS][GB_ROM_PATH_MAX];
static SeSetting g_era;
static unsigned s_mask = 0, s_str = 3, s_dur = 3;
static unsigned rmbl_get_mask(void) { return s_mask; }
static void rmbl_set_mask(unsigned m) { s_mask = m; }
static int rmbl_get_strength(void) { return (int)s_str; }
static void rmbl_set_strength(int l) { s_str = l < 1 ? 1 : (l > 5 ? 5 : l); }
static int rmbl_get_duration(void) { return (int)s_dur; }
static void rmbl_set_duration(int l) { s_dur = l < 1 ? 1 : (l > 5 ? 5 : l); }
static void rmbl_pause(void) {} static void rmbl_resume(void) {}
static bool app_can_edit(void) { return true; }
static int gb_gen_slot(uint8_t g) { return g == PDNA_GEN1 ? 3 : g == PDNA_GEN2 ? 4 : -1; }
static bool app_rom_path_set(PkGame g, const char* p) { if (strlen(p) >= GB_ROM_PATH_MAX) return false; strcpy(g_rom_path[(int)g], p); return true; }
static bool app_gb_rom_path_set(uint8_t g, const char* p) { if (strlen(p) >= GB_ROM_PATH_MAX) return false; strcpy(g_rom_path[gb_gen_slot(g)], p); return true; }
static int n_log = 0;
static void log_line(const char* f, ...) { (void)f; n_log++; }
static const char* sf_status_str(SfStatus s) { (void)s; return "x"; }
static SfStatus sf_write_verified(const char* path, const uint8_t* d, uint32_t n) {
  (void)path; if (n >= sizeof mem_file) return SF_FAIL; memcpy(mem_file, d, n); mem_len = (int)n; mem_exists = true; return SF_OK; }
static FRESULT f_open(FIL* f, const char* p, int m) { (void)p; (void)m; if (!mem_exists) return FR_NO_FILE; f->pos = 0; return FR_OK; }
static FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { int left = mem_len - f->pos; UINT k = (UINT)(left < (int)n ? left : (int)n); memcpy(b, mem_file + f->pos, k); f->pos += (int)k; *br = k; return FR_OK; }
static void f_close(FIL* f) { (void)f; }
static FRESULT f_opendir(DIR* d, const char* p) { (void)d; (void)p; return FR_OK; }
static void f_closedir(DIR* d) { (void)d; }
'''

DRV = r'''
static int fails = 0;
#define CHK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
static void reset(void) {
  g_sort = SORT_NAME; g_sortrev = g_show_all = g_show_hidden = g_yard_visitors = g_rom_art_off = false;
  g_backup_mode = g_pc_last_box = gb_scale_mode = 0; g_anim_mask = 0x0Bu; strcpy(g_cwd, "/");
  memset(g_rom_path, 0, sizeof g_rom_path); memset(&g_era, 0, sizeof g_era); s_mask = 0; s_str = s_dur = 3;
  mem_exists = false; mem_len = 0; memset(mem_file, 0, sizeof mem_file);
}
static void put(const char* t) { size_t n = strlen(t); memcpy(mem_file, t, n); mem_len = (int)n; mem_exists = true; }
static bool asked_with(const char* t) { reset(); put(t); return rom_welcome_asked(); }

int main(void) {
  /* 1. welcome adds ONLY romask=1 */
  reset(); cfg_save();
  char before[4096]; int blen = mem_len; memcpy(before, mem_file, (size_t)mem_len); before[blen] = 0;
  CHK(!rom_welcome_asked(), "fresh default config reads unanswered");
  CHK(strstr(before, "romask") == NULL, "a default config has no romask key (old files stay byte-identical)");
  cfg_save_ex("romask", "1", NULL);
  char after[4096]; memcpy(after, mem_file, (size_t)mem_len); after[mem_len] = 0;
  CHK(rom_welcome_asked(), "after the welcome the key reads answered");
  { char* at = strstr(after, "romask=1\n"); CHK(at != NULL, "romask=1 line present");
    if (at) { char rest[4096]; size_t pre = (size_t)(at - after); memcpy(rest, after, pre); strcpy(rest + pre, at + 9);
      CHK(strcmp(rest, before) == 0, "the welcome adds ONLY romask=1 (everything else byte-identical)"); } }
  /* 3. answered card stays answered across an unrelated save */
  g_backup_mode = 2; g_yard_visitors = true; cfg_save();
  CHK(rom_welcome_asked(), "answered card stays answered across an unrelated cfg_save");
  CHK(strstr(mem_file, "romask=1\n") != NULL, "romask=1 survives in the file");
  /* 4. unanswered readings */
  CHK(!asked_with("romask=0\n"), "romask=0 is unanswered");
  CHK(!asked_with(""), "empty file is unanswered");
  CHK(!asked_with("romask=\n"), "romask= (no value) is unanswered");
  CHK(!asked_with("romask\n"), "romask (no =) is unanswered");
  CHK(!asked_with("romask=x\n"), "romask=x is unanswered");
  CHK(!asked_with("xromask=1\n"), "xromask=1 is not our key");
  CHK(asked_with("dir=/\nromask=1\n"), "romask=1 after other keys reads answered");
  reset(); CHK(!rom_welcome_asked(), "absent file is unanswered");
  reset(); put("romask=0\nanim=3\n"); cfg_load(); cfg_save();
  CHK(strstr(mem_file, "romask") == NULL, "romask=0 is never written back as answered");
  /* 2. every key, non-default, byte-identical through cfg_load + cfg_save */
  reset();
  /* era lines come from the real sprite_era code so the expected text cannot drift from it */
  SeSetting e; memset(&e, 0, sizeof e);
  CHK(se_config_apply(&e, "era_em_party", "rs") && se_config_apply(&e, "era_g1_sum", "g2") && se_config_apply(&e, "era_fr_bank", "em"), "era keys apply");
  char eras[512]; bool tr = false; int en = se_config_write(&e, eras, (int)sizeof eras, &tr); eras[en] = 0;
  CHK(en > 0 && !tr, "era lines written");
  char full[2048];
  snprintf(full, sizeof full,
    "dir=/Saves/Gen3\nsort=2\nrev=1\nall=1\nhidden=1\nanim=%u\nrumble=21\nrstr=4\nrdur=2\npcbox=7\nyard=1\nbak=2\nromoff=1\ngbscale=1\n"
    "romask=1\ndir_rom=/Roms\ndir_gb=/GB\ndir_gbsav=/GBsav\nromrs=/Roms/Ruby.gba\nromem=/Roms/Emerald.gba\nromfr=/Roms/FireRed.gba\n"
    "romgb1=/GB/Red.gb\nromgb2=/GB/Crystal.gbc\n%s", (1u << 24) | 5u, eras);
  reset(); put(full);
  /* dir_* keys are harvested from the file by cfg_save_ex itself; the rest is applied by cfg_load */
  cfg_load();
  CHK(s_str == 4 && s_dur == 2, "cfg_load applies rstr/rdur (rumble strength 4, duration 2)");
  cfg_save();
  { char got[4096]; memcpy(got, mem_file, (size_t)mem_len); got[mem_len] = 0;
    if (strcmp(got, full) != 0) { printf("--- expected ---\n%s--- got ---\n%s", full, got); }
    CHK(strcmp(got, full) == 0, "a config holding EVERY key round-trips byte-identical through cfg_load + cfg_save"); }
  /* rstr/rdur edge: out-of-range clamps, never crashes */
  reset(); put("rstr=99\nrdur=0\n"); cfg_load(); CHK(s_str == 5 && s_dur == 1, "rstr=99 clamps to 5, rdur=0 to 1");
  printf(fails ? "RESULT: %d FAILED\n" : "RESULT: ALL PASS\n", fails);
  return fails ? 1 : 0;
}
'''


def extract():
    t = (SRC / "pdna_main.c").read_text(encoding="utf-8")
    i = t.index("#define CFG_PATH")
    j = t.index("static const char* sort_label(void)")
    return t[i:j]


def build_run(block, tmp, tag):
    c = tmp / f"cfg_{tag}.c"
    c.write_text(PRE + block + DRV, encoding="utf-8")
    exe = tmp / f"cfg_{tag}"
    cc = subprocess.run(["cc", "-std=gnu11", "-w", "-O0", "-I", str(SRC), str(c), str(SRC / "sprite_era.c"), "-o", str(exe)],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        return None, cc.stderr[-1500:]
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    return r, ""


def main():
    if not shutil.which("cc"):
        print("host_romhint_cfg_test: SKIP (no cc)")
        return 0
    blk = extract()
    mutants = {}
    m3 = blk.replace("&& v[0] == '1';", "&& v[0] != '1';", 1)
    mutants["M3 rom_welcome_asked inverted"] = m3
    key = "*romask = find_key_in_text(buf, br, \"romask\", ask, (int)sizeof ask) && ask[0] == '1';"
    mutants["M4 old-key harvest forces *romask=false"] = blk.replace(key, key + "\n  *romask = false;", 1)
    mutants["M9 cfg_load ignores rstr/rdur (the pre-fix parser)"] = re.sub(
        r'      else if \(!strcmp\(k, "rstr"\)\).*\n      else if \(!strcmp\(k, "rdur"\)\).*\n', "", blk, count=1)
    fails = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        r, err = build_run(blk, tmp, "real")
        if r is None:
            print("host_romhint_cfg_test: the extracted CFG block did not compile:\n" + err); return 1
        print(r.stdout.strip())
        if r.returncode != 0 or "ALL PASS" not in r.stdout:
            fails.append("real source: harness not green")
        for name, mb in mutants.items():
            if mb == blk:
                fails.append("mutant is a no-op: " + name); continue
            rm, e2 = build_run(mb, tmp, re.sub(r"\W", "_", name)[:12])
            if rm is None:
                fails.append(f"mutant did not compile ({name}): {e2}"); continue
            red = [l for l in rm.stdout.splitlines() if l.startswith("FAIL")]
            if rm.returncode == 0:
                fails.append("harness stays GREEN on mutant: " + name)
            else:
                print(f"  RED on {name}: {len(red)} failing check(s), e.g. {red[0] if red else rm.stdout.strip().splitlines()[-1]}")
    if fails:
        for f in fails: print("FAIL:", f)
        return 1
    print("host_romhint_cfg_test: harness green on source and red on every mutant")
    return 0


if __name__ == "__main__":
    sys.exit(main())
