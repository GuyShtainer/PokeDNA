#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_write_gate_test.py -- structural guard for hard rule 4 (writes are
Omega-only) over the whole Game Boy (Gen-1/2) edit surface in source/pdna_gen12.c
(BACKLOG b160, the write-safety-gap review).

Pure-text checks, no mgba, no build -- this is the same "grep the shipped source,
don't re-type your own copy of it" posture tests/textfit_mutation_check.py proves
for the layout tests, applied here to a write-safety gate instead of a pixel budget.

Four checks:

  (a) gb_nav_from_start() (the GB nav-menu dispatcher): every `pdna_gbXXX(gs, ...)`
      / `pdna_gbdaycare(gs, ...)` call inside it that passes a second (can_edit-
      shaped) argument must spell `app_can_edit()` somewhere on that line. This is
      the class of bug b160 fixed -- NV_TRAINER/NV_BAG/NV_CLOCK used to pass the
      bare resident-session flag `ed`, letting an EverDrive/bad-ROM-check/hack
      session edit and then write. Calls with only ONE argument (`gs` alone, e.g.
      the read-only map screens) are not can_edit dispatches and are skipped.

  (b) every mutating hook named in k_gb_ops_gen1 / k_gb_ops_gen2 (the AppSrcOps
      tables gb_session_core wires up -- move/release/paste/create/dup/daycare/
      item/export_one; .edit is unwired in both tables, so there is nothing to
      check there) must reach `app_can_edit(` or `gb_locate(` somewhere in its own
      function body -- gb_locate() itself calls app_can_edit() (pdna_gen12.c
      gate 1), so a hook that defers to it is still covered.

  (c) BACKLOG #150 S150-14: every function named in NAMED_WRITE_HOOKS (today just
      gb_native_summary_open, the native-Bank-cell EDIT entry point -- it is not a
      k_gb_ops_* hook, so check (b) never sees it, and it takes no `gs`, so check
      (a) never sees it either) must reach `app_can_edit(` somewhere in its own
      function body. gb_native_summary_open has no `gb_locate()` fallback (decision
      10: there is no GB save mounted for a Bank cell, so gb_locate() is the WRONG
      gate here) -- the gate must be app_can_edit( itself, not either-or.

  (d) BACKLOG #171b (lane s150-4-5b, post-#171 review finding): pdna_gen12_source()
      assigns `s.xfer = &k_gb_xfer;` (comment-stripped) -- start_carry() (pdna_box.c
      :1077) gates the whole lift_up path on `src->xfer`, a per-BoxSource field, never
      on the file-static `s_xfer_peer` a Bank visit installs. Without this the whole
      UP-lift feature (BACKLOG #150 S150-4/5) silently falls back to a plain memcpy on
      every grab -- no origin prompt, no pack, no serial -- proven live: a "successful"
      grab on a vehicle where every SD write fails is itself the proof lift_up never ran.

Run directly:

    python3 tests/host_gb_write_gate_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (b160).
"""
import re
import sys
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source" / "pdna_gen12.c"

# Regex for a `pdna_gbXXX(` or `pdna_gbdaycare(` call whose first argument is
# `gs` (or, for the one daycare call outside gb_nav_from_start that uses the
# session directly, `&g_ed->s`) followed by a comma -- i.e. it takes a SECOND
# argument, the can_edit-shaped one this check cares about. Calls with only one
# argument (the read-only map screens, pdna_gbmap_gen1(gs)/pdna_gbmap_gen2(gs))
# do not match this pattern and are correctly skipped.
DISPATCH_RE = re.compile(r"pdna_gb\w*\(\s*(?:gs|&g_ed->s)\s*,")

# The mutating AppSrcOps fields to check, read straight out of the two table
# literals rather than hardcoded, so a future field added to either table is
# picked up automatically. `.edit` is deliberately excluded: both tables set it
# to 0 (see pdna_gen12.c's own comment on gb_edit_hook being retired), so there
# is no function to check for it.
TABLE_NAMES = ("k_gb_ops_gen1", "k_gb_ops_gen2", "k_gb_xfer")
FIELD_RE = re.compile(r"\.(\w+)\s*=\s*(\w+)")
# BACKLOG #150 S150-7: `gen` is a uint8_t (not a function, never mutates anything);
# `preview_down` is a pure read-only preview (no write, per its own header comment);
# `move_within` is unimplemented (NULL) in this lane -- none of the three need a gate.
SKIP_FIELDS = {"edit", "copy_native", "editable", "gen", "preview_down", "move_within"}
# (`view` stays IN: gb_view_hook computes can_edit = app_can_edit() && ... and hands it to an
#  editable summary -- b160 re-verify R5.)

# BACKLOG #150 S150-14, check (c): named write hooks outside the k_gb_ops_* tables and
# outside gb_nav_from_start's dispatch -- checked individually by name, not by table scan.
NAMED_WRITE_HOOKS = ("gb_native_summary_open",
                     # BACKLOG #150 S150-12: the read-only mount's COPY-flavoured
                     # lift -- installed as BoxXferOps.lift_up on k_gb_xfer_ro, which
                     # check (b)'s table scan does not cover (TABLE_NAMES lists only
                     # k_gb_ops_gen1/gen2/k_gb_xfer, not k_gb_xfer_ro). It persists
                     # (pdna_bank_next_serial() writes bank.meta through the shared
                     # gb_lift_pack() body it calls), so it needs its own
                     # app_can_edit( in ITS OWN body -- the checker scans each named
                     # function's own text, never the shared callee it delegates to.
                     "gb_lift_copy_hook",
                     # BACKLOG #150 S150-8 step 7: the two DOWN-converting arms --
                     # neither is a k_gb_ops_* table hook (check (b) never sees
                     # them) and neither takes a `gs` argument (check (a) never
                     # sees them either). gb_locate() is the wrong gate for
                     # BOTH (the Gen-3 arm has no GB save mounted at all; the
                     # bridge arm's destination box comes straight from the
                     # dispatcher, not from a gb_locate() lookup) -- app_can_edit(
                     # must appear in each body directly, same reasoning as
                     # gb_native_summary_open's own row above.
                     "gb_bank_down_gen3", "gb_bank_down_bridge")


def strip_comments(text: str) -> str:
    """Blank out /* ... */ and // comments but keep every newline, so line numbers
    reported by the callers still point at the real source line. b160 re-verify R1:
    the F2 fix's own comment mentions gb_locate() in prose, which kept check (b)
    green after the gate itself was deleted -- tokens must be matched in CODE only."""
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    """Return the full body (signature through matching close-brace) of the
    top-level function `func_name` defined in `text`, or "" if not found. Brace-
    depth walk, not a fixed-line-count slice -- robust to the function growing or
    shrinking as other b160 items land."""
    m = re.search(r"^\w[\w \*]*\b" + re.escape(func_name) + r"\s*\([^;]*?\)\s*\{",
                  text, re.MULTILINE)
    if not m:
        # static functions: the return type + name may not start the line (e.g.
        # `static bool\ngb_foo(...)`), so retry with a looser anchor: just the
        # name followed by `(` and, eventually, an opening brace before the next
        # top-level function.
        m = re.search(r"\b" + re.escape(func_name) + r"\s*\([^;{]*\)\s*\{", text)
        if not m:
            return ""
    start = m.start()
    depth = 0
    i = m.end() - 1  # position of the opening brace
    assert text[i] == "{"
    depth = 1
    j = i + 1
    while j < len(text) and depth > 0:
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
        j += 1
    return text[start:j]


def check_nav_dispatch(text: str) -> list[str]:
    """Returns a list of violation strings (empty == pass)."""
    body = extract_function_body(text, "gb_nav_from_start")
    if not body:
        return ["gb_nav_from_start() not found in source/pdna_gen12.c"]
    body = strip_comments(body)   # R5: a trailing `/* app_can_edit() ... */` must not satisfy the check
    violations = []
    for lineno, line in enumerate(body.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith("//"):
            continue  # comment lines mention pdna_gbXXX( too -- code only
        if DISPATCH_RE.search(line) and "app_can_edit()" not in line:
            violations.append(f"gb_nav_from_start body line {lineno}: {stripped!r} "
                               f"passes a can_edit-shaped argument without app_can_edit()")
    return violations


def gate_table_fields(text: str) -> dict[str, str]:
    """field name -> hook function name, unioned over both k_gb_ops_* tables,
    mutating fields only (SKIP_FIELDS + zero-valued fields dropped)."""
    fields: dict[str, str] = {}
    for tbl in TABLE_NAMES:
        m = re.search(r"\b" + tbl + r"\s*=\s*\{(.*?)\};", text, re.DOTALL)
        if not m:
            continue
        for fname, fval in FIELD_RE.findall(m.group(1)):
            if fname in SKIP_FIELDS or fval == "0":
                continue
            fields[fname] = fval
    return fields


def check_mutating_hooks(text: str) -> list[str]:
    fields = gate_table_fields(text)
    if not fields:
        return ["no mutating fields found in k_gb_ops_gen1/k_gb_ops_gen2 -- table regex broken"]
    violations = []
    for fname, func in sorted(fields.items()):
        body = extract_function_body(text, func)
        if not body:
            violations.append(f".{fname} = {func}(): function body not found")
            continue
        code = strip_comments(body)
        if "app_can_edit(" not in code and "gb_locate(" not in code:
            violations.append(f".{fname} = {func}(): body has neither app_can_edit( nor gb_locate( (comments excluded)")
    return violations


def check_named_write_hooks(text: str) -> list[str]:
    """BACKLOG #150 S150-14, check (c): every function in NAMED_WRITE_HOOKS must
    reach `app_can_edit(` in its own body (comments excluded) -- gb_locate() is
    NOT an acceptable alternative here (decision 10: no GB save is mounted for a
    Bank cell, so gb_locate() is the wrong gate)."""
    violations = []
    for func in NAMED_WRITE_HOOKS:
        body = extract_function_body(text, func)
        if not body:
            violations.append(f"{func}(): function body not found")
            continue
        code = strip_comments(body)
        if "app_can_edit(" not in code:
            violations.append(f"{func}(): body has no app_can_edit( (comments excluded)")
    return violations


def check_xfer_wired(text: str) -> list[str]:
    """BACKLOG #171b (lane s150-4-5b, post-#171 review finding): pdna_gen12_source()
    must assign `s.xfer` to `&k_gb_xfer` (comments excluded) -- start_carry() (pdna_box.c
    :1077) gates the whole lift_up path on `src->xfer`, a per-BoxSource FIELD, never on
    the file-static `s_xfer_peer` a Bank visit installs via pdna_box_xfer_set(). Without
    this assignment every GB-scope grab falls straight to start_carry()'s plain memcpy
    branch -- BoxXferOps.lift_up (the origin prompt, the pack, pdna_bank_next_serial())
    NEVER RUNS, confirmed by lane s150-4-5b's own per-tap mGBA trace: a "successful"
    grab on a vehicle where every SD write fails is proof by itself (had lift_up run,
    next_serial -> meta_save would have failed and the grab would have been refused).
    BACKLOG #150 S150-12: the assignment is now a ternary (`pdna_gen12_resident() ?
    &k_gb_xfer : &k_gb_xfer_ro`), since the read-only mount installs a second,
    delete-free table -- `&k_gb_xfer` must still appear somewhere on the `s.xfer =`
    line, in either the bare or the ternary form."""
    body = strip_comments(extract_function_body(text, "pdna_gen12_source"))
    if not body:
        return ["pdna_gen12_source(): function body not found"]
    xfer_line = re.search(r"\bs\.xfer\s*=[^;]*;", body)
    if not xfer_line or not re.search(r"&k_gb_xfer\s*[;:]", xfer_line.group(0)):
        return ["pdna_gen12_source(): no `s.xfer = ... &k_gb_xfer ...;` assignment in its "
                "(comment-stripped) body -- start_carry()'s src->xfer gate would stay "
                "NULL and BoxXferOps.lift_up would never run"]
    return []


def check_native_unpack_in_loop(text: str) -> list[str]:
    """S150-14 decision 3: bc_unpack must sit INSIDE gb_native_summary_open's for(;;),
    never hoisted above it -- pdna_gbsummary edits `e` in place and restores nothing."""
    body = strip_comments(extract_function_body(text, "gb_native_summary_open"))
    if not body:
        return ["gb_native_summary_open(): function body not found"]
    if "for (;;)" not in body or body.index("for (;;)") > body.index("bc_unpack("):
        return ["gb_native_summary_open(): bc_unpack( is hoisted above the for(;;) "
                "-- decision 3's discard trap is re-introduced"]
    return []


def check_accept_down_rollback(text: str) -> list[str]:
    """REVIEW F1 (BACKLOG #150 S150-7): gb_accept_down_hook()'s 10(c) party-full offer
    (gb_accept_down_party_deposit()) commits a party->box move into the RESIDENT image
    (gbs_move -> gbs_commit_list) before this hook has finished its own pre-flight --
    a declined confirm or a reload failure past that point must not leave that move
    sitting in the image un-rolled-back (the NEXT gb_persist() anywhere, even an
    unrelated box rename, would write it to the card). Every `return false;` in the
    function body AFTER the gb_accept_down_party_deposit( call must have a
    `gb_rollback(` earlier in its own enclosing brace block (comment-stripped)."""
    body = extract_function_body(text, "gb_accept_down_hook")
    if not body:
        return ["gb_accept_down_hook() not found in source/pdna_gen12.c"]
    code = strip_comments(body)
    # Anchored on the WHOLE `if (!gb_accept_down_party_deposit(...)) return false;`
    # statement, not just the call's opening paren -- that specific `return false;`
    # needs no gb_rollback() of its own (gb_accept_down_party_deposit()'s own contract
    # is "nothing touched" on a false return, same as every other pre-flight refusal in
    # this hook); the invariant only starts to matter for what runs AFTER the deposit
    # actually SUCCEEDED.
    dm = re.search(r"gb_accept_down_party_deposit\([^)]*\)\)\s*return\s+false\s*;", code)
    if not dm:
        return ["gb_accept_down_party_deposit(...)) return false; statement not found "
                "in gb_accept_down_hook -- check is stale, update it"]
    tail = code[dm.end():]

    events = [(m.start(), "open") for m in re.finditer(r"\{", tail)]
    events += [(m.start(), "close") for m in re.finditer(r"\}", tail)]
    events += [(m.start(), "return") for m in re.finditer(r"\breturn\s+false\s*;", tail)]
    events.sort(key=lambda e: e[0])

    violations = []
    stack = [0]   # block-start offsets into `tail`; 0 = the implicit outer block
    for pos, kind in events:
        if kind == "open":
            stack.append(pos)
        elif kind == "close":
            if len(stack) > 1:
                stack.pop()
        else:
            block_start = stack[-1]
            window = tail[block_start:pos]
            if "gb_rollback(" not in window:
                lineno = tail[:pos].count("\n") + 1
                violations.append(
                    f"gb_accept_down_hook: a `return false;` after the 10(c) deposit "
                    f"call (tail line {lineno}) has no gb_rollback( earlier in its own "
                    f"enclosing block -- an un-consented change could reach the card")
    return violations



def check_make_legal_stamp(text: str) -> list[str]:
    """S150-8b review D1/R2: gb_bank_down_gen3's MAKE-LEGAL branch must stamp the corrected
    level into the ledger's `written` record (`gb_set_level(&written, to_lvl)`) AFTER the
    Gen-3 edit is committed, or a later restore reads the correction as an in-game level-up."""
    body = extract_function_body(text, "gb_bank_down_gen3")
    if not body:
        return ["gb_bank_down_gen3() not found in source/pdna_gen12.c"]
    body = strip_comments(body)
    i = body.find("if (ch == GB_XFER_MAKE_LEGAL) {")
    if i < 0:
        return ["gb_bank_down_gen3(): no MAKE-LEGAL branch found"]
    j = body.find("}", i)
    branch = body[i:j]
    if "gen3_edit_commit(&em, out80);" not in branch:
        return ["gb_bank_down_gen3(): the MAKE-LEGAL branch no longer commits the Gen-3 edit"]
    k = branch.find("gb_set_level(&written, to_lvl)")
    if k < 0 or k < branch.find("gen3_edit_commit(&em, out80);"):
        return ["gb_bank_down_gen3(): the MAKE-LEGAL branch does not stamp gb_set_level(&written, to_lvl) "
                "after gen3_edit_commit -- a restore would read the correction as a level-up (S150-8b D1)"]
    return []

def run_all(path: Path) -> list[str]:
    text = path.read_text()
    return (check_nav_dispatch(text) + check_mutating_hooks(text) + check_named_write_hooks(text)
            + check_native_unpack_in_loop(text) + check_xfer_wired(text) + check_accept_down_rollback(text)
            + check_make_legal_stamp(text))


def main() -> int:
    if not SRC.exists():
        print("SKIP (source/pdna_gen12.c not found)")
        return 0

    # --- real check against the shipped source ---------------------------------
    violations = run_all(SRC)
    fields = gate_table_fields(SRC.read_text())
    print(f"gb_nav_from_start dispatch check: {'ok' if not any('gb_nav_from_start' in v or 'app_can_edit()' in v for v in violations) else 'see violations below'}")
    print(f"mutating hooks checked ({len(fields)}): " + ", ".join(f".{k}={v}" for k, v in sorted(fields.items())))
    print(f"named write hooks checked ({len(NAMED_WRITE_HOOKS)}): " + ", ".join(NAMED_WRITE_HOOKS))
    print(f"BACKLOG #171b xfer-wired check: {'ok' if not check_xfer_wired(SRC.read_text()) else 'see violations below'}")
    if violations:
        print("FAIL -- shipped source/pdna_gen12.c has a write-gate gap:")
        for v in violations:
            print(f"  FAIL: {v}")
        return 1
    print("ok: shipped source/pdna_gen12.c -- every can_edit dispatch, every mutating "
          "AppSrcOps hook and every named write hook reaches app_can_edit()/gb_locate()")

    # --- self-mutation proof: revert ONE dispatch to bare `ed`, must go red ----
    tmpdir = Path(tempfile.mkdtemp(prefix="gbwritegate_"))
    try:
        scratch = tmpdir / "pdna_gen12.c"
        shutil.copyfile(SRC, scratch)
        original = scratch.read_text()
        target = "if (gs) pdna_gbtrainer(gs, ed && app_can_edit());"
        mutated_line = "if (gs) pdna_gbtrainer(gs, ed);"
        if target not in original:
            print(f"FAIL -- self-mutation target line not found verbatim: {target!r} "
                  f"(source drifted -- update this test's target string)")
            return 1
        mutated = original.replace(target, mutated_line, 1)
        scratch.write_text(mutated)
        mutation_violations = run_all(scratch)
        if not mutation_violations:
            print("FAIL -- self-mutation check: reverting NV_TRAINER's dispatch to bare "
                  "`ed` did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation check: reverting NV_TRAINER's dispatch to bare `ed` -- "
              "correctly caught:")
        for v in mutation_violations:
            print(f"  (mutated-copy) FAIL: {v}")
        # --- second self-mutation (b160 re-verify R1): delete the F2 gate block at the top
        # of gb_create_hook. Its own comment mentions gb_locate() in prose, so a
        # comment-blind check stayed green here -- this case pins the comment-stripping.
        create_body = extract_function_body(original, "gb_create_hook")
        gm = re.search(r"  if \(!app_can_edit\(\)\) \{\n(?:.*?\n)*?  \}\n", create_body)
        if not create_body or not gm:
            print("FAIL -- second self-mutation target (gb_create_hook's app_can_edit gate) not found "
                  "(source drifted -- update this test)")
            return 1
        mutated2 = original.replace(create_body, create_body.replace(gm.group(0), "", 1), 1)
        scratch.write_text(mutated2)
        mutation2 = [v for v in run_all(scratch) if ".create" in v]
        if not mutation2:
            print("FAIL -- second self-mutation check: deleting gb_create_hook's app_can_edit() gate "
                  "did NOT turn this test red (comment-blind check)")
            return 1
        print("self-mutation check 2: deleting gb_create_hook's gate -- correctly caught:")
        for v in mutation2:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- third self-mutation (BACKLOG #150 S150-14): drop `&& app_can_edit()`
        # from gb_native_summary_open's own gate line, must go red.
        target3 = "const bool can_edit = allow_edit && out80 && app_can_edit();"
        mutated_line3 = "const bool can_edit = allow_edit && out80;"
        if target3 not in original:
            print(f"FAIL -- third self-mutation target line not found verbatim: {target3!r} "
                  f"(source drifted -- update this test's target string)")
            return 1
        mutated3 = original.replace(target3, mutated_line3, 1)
        scratch.write_text(mutated3)
        mutation3 = [v for v in run_all(scratch) if "gb_native_summary_open" in v]
        if not mutation3:
            print("FAIL -- third self-mutation check: dropping `&& app_can_edit()` from "
                  "gb_native_summary_open's gate line did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation check 3: dropping `&& app_can_edit()` from "
              "gb_native_summary_open's gate line -- correctly caught:")
        for v in mutation3:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- fourth self-mutation (BACKLOG #171b): delete `s.xfer = &k_gb_xfer;` from
        # pdna_gen12_source(), the exact regression the reviewer found live -- must go red.
        target4 = ("  s.xfer       = pdna_gen12_resident() ? &k_gb_xfer : &k_gb_xfer_ro;   "
                    "/* BACKLOG #171b: start_carry reads src->xfer, not s_xfer_peer */\n")
        if target4 not in original:
            print(f"FAIL -- fourth self-mutation target line not found verbatim: {target4!r} "
                  f"(source drifted -- update this test's target string)")
            return 1
        mutated4 = original.replace(target4, "", 1)
        scratch.write_text(mutated4)
        mutation4 = [v for v in run_all(scratch) if "pdna_gen12_source" in v]
        if not mutation4:
            print("FAIL -- fourth self-mutation check: deleting `s.xfer = &k_gb_xfer;` did "
                  "NOT turn this test red (BACKLOG #171b's own regression would ship silently)")
            return 1
        print("self-mutation check 4: deleting `s.xfer = &k_gb_xfer;` from "
              "pdna_gen12_source() -- correctly caught:")
        for v in mutation4:
            print(f"  (mutated-copy) FAIL: {v}")

        # --- fifth self-mutation (REVIEW F1, BACKLOG #150 S150-7): drop the
        # gb_rollback() a declined confirm needs after the 10(c) deposit, must go red.
        target5 = ("if (!app_confirm(PDNA_XFER_DOWN_CONFIRM_TITLE, cl1)) "
                   "{ gb_rollback(); return false; }")
        mutated_line5 = "if (!app_confirm(PDNA_XFER_DOWN_CONFIRM_TITLE, cl1)) { return false; }"
        if target5 not in original:
            print(f"FAIL -- fifth self-mutation target line not found verbatim: {target5!r} "
                  f"(source drifted -- update this test's target string)")
            return 1
        mutated5 = original.replace(target5, mutated_line5, 1)
        scratch.write_text(mutated5)
        mutation5 = [v for v in run_all(scratch) if "gb_accept_down_hook" in v]
        if not mutation5:
            print("FAIL -- fifth self-mutation check: dropping gb_rollback() from the "
                  "declined-confirm return did NOT turn this test red (vacuous check)")
            return 1
        print("self-mutation check 5: dropping gb_rollback() from the declined-confirm "
              "return in gb_accept_down_hook -- correctly caught:")
        for v in mutation5:
            print(f"  (mutated-copy) FAIL: {v}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\nhost_gb_write_gate_test: ok (shipped source clean, all five mutations caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
