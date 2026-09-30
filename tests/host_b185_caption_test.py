#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_b185_caption_test.py -- BACKLOG #291 text pin for run_b185_cold_locate's captions (no build, no mGBA).

  (a) cap1/cap2 end with "served by {served1|2}" so a TABLE/CACHE hit's caption stops claiming a cold scan;
  (b) the "KB/s floor" text is emitted by a helper that returns "" unless the source starts with SCAN
      (the two helpers are extracted from tools/dgb_shots.py by AST and exercised directly);
  (c) neither caption nor the console line spells the floor inline any more (the helper is the only path).
"""
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
text = (ROOT / "tools" / "dgb_shots.py").read_text(encoding="utf-8")
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


tree = ast.parse(text)
ns = {}
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name in ("_floor_text", "_caption_floor"):
        exec(compile(ast.Module([node], []), "dgb_shots", "exec"), ns)
check("_floor_text" in ns and "_caption_floor" in ns, "helpers _floor_text/_caption_floor missing")
if not fails:
    for fn in ("_floor_text", "_caption_floor"):
        f = ns[fn]
        check("floor" in f("SCAN", 20480.0), f"{fn}: SCAN must print the floor")
        check("floor" in f("SCAN (full locate)", 20480.0), f"{fn}: SCAN-prefixed source prints the floor")
        for src in ("TABLE", "CACHE", "UNKNOWN (no cold-locate line seen)"):
            check(f(src, 20480.0) == "", f"{fn}: {src} must NOT print a floor")

m = re.search(r"def run_b185_cold_locate.*?\n    return ok, \[\]", text, re.S)
check(m is not None, "run_b185_cold_locate not found")
body = m.group(0) if m else ""
for n in ("1", "2"):
    check(re.search(r"cap%s = \(.*?served by \{served%s\}\"\)" % (n, n), body, re.S) is not None,
          f"cap{n} must end with 'served by {{served{n}}}'")
check("KB/s floor\")" not in body.replace("_floor_text", ""), "an inline unconditional KB/s floor is back")
check(len(re.findall(r"floor\"", body)) == 0, "floor text must come only from the helpers")

if fails:
    print("FAIL host_b185_caption_test:")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("host_b185_caption_test: ok")
