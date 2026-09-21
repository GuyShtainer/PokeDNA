#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_browser_parity_test.py -- BACKLOG #186: "the ROM picker must BE the save
browser." Pins that the launch (.sav) browser and the three pdna_map.c pickers
(app_pick_rom/app_pick_gb_save/app_pick_gb_rom) are now literally the SAME core
(browse_pick_spec(), source/pdna_main.c) instead of two separate implementations,
and that the old minimal picker (pick_rom()) is gone.

Structural checks over the REAL shipped source (source/pdna_main.c + source/pdna_map.c),
not a second, driftable description of the design:
  1. pick_rom() no longer exists anywhere in the tree (A2).
  2. browse_pick_spec() exists and is the ONE definition every caller below reaches.
  3. All four call sites reach it: browse_pick() (the .sav instance), app_pick_rom(),
     app_pick_gb_save(), app_pick_gb_rom().
  4. Each of the four specs carries its own title / extension filter / cfg_key, and
     the extension lists are genuinely different per kind (not four aliases of one
     array -- a mutant that pointed every spec at the same list would still "call
     browse_pick_spec" and pass a naive existence check).
  5. Exactly one spec (the .sav one) sets menu_extra true (Verify ROM / Reboot are
     launch-browser-only, A1).
  6. The footer string is defined ONCE, not per-kind (the chrome is the core's).
  7. cfg_key coverage: "dir" (unchanged, A5) plus the three NEW keys A1 designed
     (dir_rom / dir_gb / dir_gbsav), each used by exactly the kind the brief says.
  8. cfg_load() recognises all three new keys (does not silently drop them into the
     era_ catch-all -- BACKLOG #186 A4).

Run directly:

    python3 tests/host_browser_parity_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #186).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN_SRC = ROOT / "source" / "pdna_main.c"
MAP_SRC = ROOT / "source" / "pdna_map.c"
MAP_HDR = ROOT / "source" / "pdna_map.h"

FAILS: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILS.append(msg)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def main() -> int:
    if not MAIN_SRC.exists() or not MAP_SRC.exists() or not MAP_HDR.exists():
        print(f"SKIP: source files not found")
        return 0

    main_raw = MAIN_SRC.read_text(errors="replace")
    main_text = strip_comments(main_raw)
    map_raw = MAP_SRC.read_text(errors="replace")
    map_text = strip_comments(map_raw)
    hdr_text = strip_comments(MAP_HDR.read_text(errors="replace"))

    # ---- 1. pick_rom() is gone -------------------------------------------------
    check(re.search(r"\bpick_rom\s*\(", map_text) is None,
          "pick_rom() must be deleted (BACKLOG #186 A2) -- found a call/definition still in pdna_map.c")
    check("PickEnt" not in map_text, "the PickEnt type must be deleted along with pick_rom()")
    check("s_pick_ext" not in map_text, "the old per-call s_pick_ext[] state must be deleted along with pick_rom()")

    # ---- 2/3. browse_pick_spec() exists, declared in pdna_map.h, defined ONCE,
    # and every one of the four call sites reaches it ---------------------------
    check("bool browse_pick_spec(const BrowseSpec* spec, char* out, int cap);" in hdr_text,
          "browse_pick_spec() must be declared in pdna_map.h (shared with pdna_map.c)")
    def_count = len(re.findall(r"\bbool\s+browse_pick_spec\s*\([^;]*\)\s*\{", main_text))
    check(def_count == 1, f"browse_pick_spec() must be defined exactly once (found {def_count})")

    # browse_pick() -- the .sav instance
    bp_body_m = re.search(r"static bool browse_pick\s*\(char\* out, int cap\)\s*\{(.*?)\n\}", main_text, re.DOTALL)
    check(bool(bp_body_m), "browse_pick() (the .sav instance) not found in pdna_main.c")
    if bp_body_m:
        bp_body = bp_body_m.group(1)
        check("browse_pick_spec(&spec, out, cap)" in bp_body or "browse_pick_spec(" in bp_body,
              "browse_pick() must call browse_pick_spec() -- it is the core's .sav instance now, not a second implementation")
        check('.cfg_key = "dir"' in bp_body, 'browse_pick()\'s spec must use cfg_key "dir" (A5: unchanged folder-memory key)')
        check(".menu_extra = true" in bp_body,
              "browse_pick()'s spec must set menu_extra = true (Verify ROM / Reboot stay launch-browser-only)")
        check(".entries = g_entries" in bp_body and ".cap = MAX_ENTRIES" in bp_body,
              "browse_pick() must hand the core its OWN resident g_entries/MAX_ENTRIES (A3: not overlaid -- "
              "see app_pick_rom's arena/mon_decomp buffer below for the kind that IS)")

    # app_pick_rom() goes through a shared rom_pick() helper (also used by pdna_map()'s
    # own inline "ask for this game's ROM" flow) instead of building its spec inline
    # like the other two wrappers -- same core either way, so its check follows the
    # one extra level of indirection rather than requiring identical shape.
    rp_m = re.search(r"static bool rom_pick\s*\([^)]*\)\s*\{(.*?)\n\}", map_text, re.DOTALL)
    check(bool(rp_m), "rom_pick() helper not found in pdna_map.c")
    if rp_m:
        rp_body = rp_m.group(1)
        check("browse_pick_spec(" in rp_body, "rom_pick() must call browse_pick_spec() (BACKLOG #186 A2)")
        check('.cfg_key = "dir_rom"' in rp_body, 'rom_pick()\'s spec must use cfg_key "dir_rom" (A1)')
        check("k_rom_exts" in rp_body, "rom_pick()'s spec must reference its own extension list k_rom_exts")
        check(".menu_extra = false" in rp_body,
              "rom_pick()'s spec must set menu_extra = false -- Verify ROM/Reboot are launch-browser-only (A1)")
    apr_m = re.search(r"\bbool\s+app_pick_rom\s*\([^)]*\)\s*\{(.*?)\n\}", map_text, re.DOTALL)
    check(bool(apr_m), "app_pick_rom() not found in pdna_map.c")
    if apr_m:
        check("rom_pick(" in apr_m.group(1), "app_pick_rom() must call the shared rom_pick() helper")

    for fn, key, exts_var in (
        ("app_pick_gb_save", "dir_gbsav", "k_gbsav_exts"),
        ("app_pick_gb_rom", "dir_gb", "k_gbrom_exts"),
    ):
        m = re.search(r"\bbool\s+" + fn + r"\s*\([^)]*\)\s*\{(.*?)\n\}", map_text, re.DOTALL)
        check(bool(m), f"{fn}() not found in pdna_map.c")
        if not m:
            continue
        body = m.group(1)
        check("browse_pick_spec(" in body, f"{fn}() must call browse_pick_spec() (BACKLOG #186 A2)")
        check(f'.cfg_key = "{key}"' in body, f'{fn}()\'s spec must use cfg_key "{key}" (A1)')
        check(exts_var in body, f"{fn}()'s spec must reference its own extension list {exts_var}")
        check(".menu_extra = false" in body,
              f"{fn}()'s spec must set menu_extra = false -- Verify ROM/Reboot are launch-browser-only (A1)")

    # ---- 4. the three extension lists are genuinely distinct arrays, not aliases --
    ext_lists = {}
    for name in ("k_rom_exts", "k_gbsav_exts", "k_gbrom_exts"):
        m = re.search(name + r"\[\]\s*=\s*\{([^}]*)\}", map_text)
        check(bool(m), f"{name} definition not found")
        if m:
            ext_lists[name] = tuple(x.strip().strip('"') for x in m.group(1).split(",") if x.strip() and x.strip() != "0")
    if len(ext_lists) == 3:
        vals = list(ext_lists.values())
        check(len(set(vals)) == 3,
              f"the three extension lists must be genuinely different, got {ext_lists} "
              "(a mutant aliasing them would still pass a naive 'browse_pick_spec is called' check)")
        check(ext_lists["k_rom_exts"] == (".gba",), f"k_rom_exts must be exactly ('.gba',), got {ext_lists['k_rom_exts']}")
        check(set(ext_lists["k_gbsav_exts"]) == {".sav", ".srm"},
              f"k_gbsav_exts must be {{'.sav','.srm'}}, got {ext_lists['k_gbsav_exts']}")
        check(set(ext_lists["k_gbrom_exts"]) == {".gb", ".gbc"},
              f"k_gbrom_exts must be {{'.gb','.gbc'}}, got {ext_lists['k_gbrom_exts']}")

    # ---- 5. exactly one spec sets menu_extra = true across the whole tree ---------
    true_count = len(re.findall(r"\.menu_extra\s*=\s*true", main_text)) + len(re.findall(r"\.menu_extra\s*=\s*true", map_text))
    check(true_count == 1, f"exactly one BrowseSpec (the .sav one) may set menu_extra = true, found {true_count}")

    # ---- 6. the footer string is defined exactly once -----------------------------
    footer_count = main_text.count('"A pick  B up  SEL sort  ST menu"')
    check(footer_count == 1,
          f"the browser footer must be a single shared string in the core, found {footer_count} occurrence(s) "
          "(one per kind would mean the chrome forked again)")

    # ---- 7/8. cfg_key coverage + cfg_load recognises the three new keys -----------
    for key in ("dir_rom", "dir_gb", "dir_gbsav"):
        check(f'"{key}"' in main_text, f'cfg key "{key}" must appear in pdna_main.c (A1/A4)')
    cfg_load_m = re.search(r"static void cfg_load\(void\)\s*\{(.*?)\n\}", main_text, re.DOTALL)
    check(bool(cfg_load_m), "cfg_load() not found")
    if cfg_load_m:
        load_body = cfg_load_m.group(1)
        for key in ("dir_rom", "dir_gb", "dir_gbsav"):
            check(f'"{key}"' in load_body, f'cfg_load() must explicitly recognise "{key}" (A4) -- not fall into the era_ catch-all')
        # The recognition must come BEFORE the se_config_apply catch-all, else a
        # mutant that deleted the explicit branches would still show the key
        # substring present (in a comment) and pass a naive check.
        era_pos = load_body.find("se_config_apply")
        for key in ("dir_rom", "dir_gb", "dir_gbsav"):
            key_pos = load_body.find(f'"{key}"')
            check(key_pos != -1 and era_pos != -1 and key_pos < era_pos,
                  f'"{key}"\'s recognition must come before the se_config_apply() catch-all in cfg_load()')

    # ---- 9. D1 fix (real regression a Fable review caught): "dir=" must never be
    # written from the LIVE g_cwd while a non-.sav picker has borrowed it for its own
    # folder -- cfg_save_ex()'s dir_val parameter is what fixes that, and this pins
    # both halves of the fix (the round-trip AND the write) rather than just "the
    # function still exists". -----------------------------------------------------
    cse_m = re.search(r"static void cfg_save_ex\s*\([^)]*\)\s*\{(.*?)\n\}", main_text, re.DOTALL)
    check(bool(cse_m), "cfg_save_ex() not found in pdna_main.c")
    if cse_m:
        cse_body = cse_m.group(1)
        check('k_dirkey[3] = { "dir_rom", "dir_gb", "dir_gbsav" }' in re.sub(r"\s+", " ", cse_body),
              'cfg_save_ex() must declare k_dirkey[3] = { "dir_rom", "dir_gb", "dir_gbsav" } '
              '(D2: all three round-tripped keys, in this exact order)')
        check('dir_val ? dir_val : g_cwd' in re.sub(r"\s+", " ", cse_body),
              'cfg_save_ex()\'s "dir=" line must read `dir_val ? dir_val : g_cwd` (D1 fix) -- '
              "a plain `g_cwd` here is exactly the regression the review caught")
        check(main_text.count("cfg_save_for(spec, is_dir_key ? 0 : saved_cwd)") == 5,
              "all five in-picker cfg_save_for() sites must pass saved_cwd (D1) -- a site passing 0 "
              "reintroduces the dir= clobber (re-verify one-liner, 2026-09-21)")

    crod_m = re.search(r"static void __attribute__\(\(noinline\)\) cfg_read_old_dirkeys\s*\([^)]*\)\s*\{(.*?)\n\}",
                        main_text, re.DOTALL)
    check(bool(crod_m), "cfg_read_old_dirkeys() not found in pdna_main.c")
    if crod_m:
        crod_body = crod_m.group(1)
        for key, out_var in (("dir_rom", "dirrom"), ("dir_gb", "dirgb"), ("dir_gbsav", "dirgbsav")):
            check(f'find_key_in_text(buf, br, "{key}", {out_var}, GB_ROM_PATH_MAX)' in re.sub(r"\s+", " ", crod_body),
                  f'cfg_read_old_dirkeys() must round-trip "{key}" into its own {out_var} output '
                  "(D2: a dropped key here is exactly what a card's OTHER two folder memories "
                  "would silently lose the next time any unrelated setting is saved)")

    if FAILS:
        print(f"host_browser_parity_test: {len(FAILS)} FAIL(s)")
        for f in FAILS:
            print(f"  !! {f}")
        return 1
    print("host_browser_parity_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
