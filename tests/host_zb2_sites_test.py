#!/usr/bin/env python3
"""Source pins for lane zb2 (BACKLOG #406 review F5): the call sites the funnel test cannot reach.

host_jrn_funnel_test.c drives jrn_app.c directly, so deleting a call in pdna_main.c passes it. These pins read the
real call sites: app_step_end flushes a completed swap pair (#406(b)); both journal-load paths (Gen-3 app_journal_load,
GB app_gb_journal_open) show the "LAST MOVE CUT OFF" note with the heartbeat paused (review F4). Each fact has a
mutant that must turn it red.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source"
checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def function_body(text: str, name: str) -> str:
    text = strip_comments(text)
    m = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", text, re.MULTILINE)
    if not m:
        return ""
    depth, j = 1, m.end()
    while j < len(text) and depth > 0:
        depth += (text[j] == "{") - (text[j] == "}")
        j += 1
    return text[m.start():j]


NOTE = re.compile(r"hb_pause\(\);\s*perf_span_pause\(\);\s*app_journal_cutoff_note\(\);\s*perf_span_resume\(\);\s*hb_resume\(\);")


def fact(step_end: str, g3_load: str, gb_open: str) -> tuple[bool, str]:
    if not re.search(r"img_scope_close\([^;]*\);\s*jrnapp_pair_flush\(\);", step_end):
        return False, "app_step_end must call jrnapp_pair_flush() after img_scope_close (#406(b))"
    for name, body in (("app_journal_load", g3_load), ("app_gb_journal_open", gb_open)):
        if not NOTE.search(body):
            return False, f"{name}: the cut-off note must be shown with the heartbeat + perf span paused (#406 review F4)"
    return True, ""


MUTS = (
    ("pair flush dropped", "step_end", "jrnapp_pair_flush();", ""),
    ("Gen-3 note dropped", "g3_load", "app_journal_cutoff_note();", ""),
    ("GB note dropped", "gb_open", "app_journal_cutoff_note();", ""),
    ("Gen-3 note unpaused", "g3_load", "hb_pause(); perf_span_pause(); app_journal_cutoff_note();", "perf_span_pause(); app_journal_cutoff_note();"),
    ("GB note unpaused", "gb_open", "hb_pause(); perf_span_pause(); app_journal_cutoff_note();", "perf_span_pause(); app_journal_cutoff_note();"),
)


def main() -> int:
    main_c = (SRC / "pdna_main.c").read_text()
    parts = {"step_end": function_body(main_c, "app_step_end"),
             "g3_load": function_body(main_c, "app_journal_load"),
             "gb_open": function_body(main_c, "app_gb_journal_open")}
    for k, v in parts.items():
        check(bool(v), f"function not found: {k}")
    ok, why = fact(**parts)
    check(ok, why)
    for label, which, old, new in MUTS:
        if old not in parts[which]:
            check(False, f"mutation anchor missing ({label})")
            continue
        p2 = dict(parts)
        p2[which] = parts[which].replace(old, new, 1)
        bad, _ = fact(**p2)
        check(not bad, f"MUT {label} was NOT caught")
    print(f"host_zb2_sites: {checks} checks, {len(fails)} failed")
    for f in fails:
        print(f"  !! FAIL: {f}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
