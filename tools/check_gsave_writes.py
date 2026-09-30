#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""check_gsave_writes.py -- BACKLOG #234 slice 0 (design fix 1): no g_save write outside the funnel.

The open Gen-3 image (`g_save`, 128 KiB) may only be WRITTEN by
  * app_stage_sections()  -- the one staging funnel (design D2; a thin wrapper over the pure-C
                             img_stage_sections in source/img_stage.c),
  * app_mark_pc_dirty()   -- the box-drop eager stage (calls img_pc_edited, same pure-C funnel),
  * app_save_finalize()   -- the checksum/verify/write tail (its fold calls img_fold_pc), or
  * a line carrying an explicit, reasoned exemption comment:   g_save-write-ok: <why>
    (the loaders that fill the image from the card / flash / fused ROM, the battle-record
    import that writes sector 31 OUTSIDE the 14-section image, the Game Boy session's own
    battery image).
Anything else -- `gen3_write_full_section(g_save,...)`, `memcpy(g_save,...)`, `g_save[i] = v`,
an `f_read`/`sf_read_full`/`flashsave_read`/`fused_sav_read` INTO g_save -- fails the build, so
the class of miss that would let a step bypass the (slice-2) journal can never silently recur.

Limits, stated honestly: this is a text check. It cannot see a write through an alias
(`uint8_t* p = g_save; p[0] = 1;`) or a callee that mutates a `g_save` it was handed
(`pdna_gen12_show_image`, `gen3_parse_into`); those are GB-session / parse paths and are
reviewed by hand. It does catch every direct spelling in the pattern list below.

Usage:  check_gsave_writes.py [source_dir]        exit 0 = clean, 1 = violations (listed).
`scan(text)` is importable so tests/host_g3_stage_sites_test.py can plant a stray write.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ALLOWED_FUNCS = ("app_stage_sections", "app_save_finalize", "app_mark_pc_dirty")
EXEMPT = "g_save-write-ok:"

# Every direct spelling of a write INTO g_save. Matched on comment-stripped text.
PATTERNS = [
    re.compile(r"\bgen3_write_full_section\s*\(\s*g_save\b"),
    re.compile(r"\b(?:memcpy|memset|memmove)\s*\(\s*g_save\b"),
    re.compile(r"\bg_save\s*\[[^\]]*\]\s*(?:=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=)"),
    re.compile(r"\bg_save\s*\[[^\]]*\]\s*(?:\+\+|--)"),
    re.compile(r"\b(?:sf_read_full|f_read|flashsave_read|fused_sav_read)\s*\([^;]*\bg_save\b"),
    re.compile(r"\bimg_(?:stage_sections|pc_edited|fold_pc)\s*\([^;]*\bg_save\b"),
    re.compile(r"\*\s*\(\s*g_save\b[^;]*\)\s*(?:=(?!=)|\+=|\|=|&=)"),
]


def strip_comments(text: str) -> str:
    """Blank /*...*/ and // comment bodies (and string-literal bodies) but keep every newline."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            body = text[i + 1:max(i + 1, j - 1)]
            out.append(c + re.sub(r"[^\n]", " ", body) + (c if j - i >= 2 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def func_ranges(clean: str) -> list[tuple[str, int, int]]:
    """(name, first_line, last_line) of each top-level function definition (1-based lines)."""
    res, depth, name, start = [], 0, None, 0
    line = 1
    head = re.compile(r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)?$")
    buf = ""
    for ch in clean:
        if ch == "{":
            if depth == 0:
                m = head.search(buf.strip()[-400:])
                name, start = (m.group(1) if m else None), line
            depth += 1
            buf = ""
        elif ch == "}":
            depth -= 1
            if depth == 0 and name:
                res.append((name, start, line))
                name = None
            buf = ""
        elif ch == ";" and depth == 0:
            buf = ""
        elif depth == 0:
            buf += ch
        if ch == "\n":
            line += 1
    return res


def scan(text: str) -> list[tuple[int, str]]:
    """Return [(line_no, source_line)] for every un-exempted write into g_save in `text`."""
    raw = text.split("\n")
    clean = strip_comments(text)
    cl = clean.split("\n")
    ranges = [r for r in func_ranges(clean) if r[0] in ALLOWED_FUNCS]
    bad = []
    for idx, line in enumerate(cl):
        no = idx + 1
        # a statement may wrap: test the line joined with the next two for the call patterns
        joined = " ".join(cl[idx:idx + 3])
        if not any(p.search(line) or (p.search(joined) and "g_save" in line) for p in PATTERNS):
            continue
        if any(a <= no <= b for _, a, b in ranges):
            continue
        ctx = "\n".join(raw[max(0, idx - 2):idx + 1])
        if EXEMPT in ctx:
            continue
        bad.append((no, raw[idx].strip()))
    return bad


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent / "source"
    nbad = 0
    for p in sorted(root.glob("*.c")):
        for no, src in scan(p.read_text(errors="replace")):
            print(f"{p}:{no}: write into g_save outside the staging funnel: {src}")
            nbad += 1
    if nbad:
        print(f"*** check_gsave_writes: {nbad} violation(s). Route the write through app_stage_sections(), "
              f"or (loaders / the sector-31 record import / GB image only) annotate it with a "
              f"'{EXEMPT} <why>' comment. See docs/briefs/234-UNDO-DESIGN.md D2/D5.")
        return 1
    print("check_gsave_writes: OK -- no g_save write outside app_stage_sections/app_save_finalize/annotated loaders")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
