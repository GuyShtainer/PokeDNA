#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_stack_budget_test.py -- unit tests for tools/stack_budget.py itself
(BACKLOG #84b, the review that found the two false-pass classes the guard's
own docstring now explains).

Not picked up by tests/run_host_tests.py (that script only builds/runs
tests/host_*_test.c files with a `cc` line in their header -- this is a pure-Python
test of a pure-Python tool, nothing to compile). Run directly:

    python3 tests/host_stack_budget_test.py

Also wired into the `stack-check` Makefile target (`make stack-check`), which runs
this file then the real guard against every already-built ELF it finds.

Four cases, each named after the defect class it guards against regressing:

  (a) estimate_frames() on a fixture reproducing mgfx_zoom_optimise's two false-pass
      shapes -- a stale `ldr rN,[pc,#imm]` from an UNRELATED earlier use must not
      leak into a later, unrelated `add sp, rN` (the review's literal reproduction:
      728 B become a wildly wrong number when a stale value is picked up). Both the
      ldr-pc/add-sp idiom and the movs/lsls/add-sp idiom independently compute the
      same 596 B frame with no explosion, and the interposed stale ldr changes
      nothing.
  (b) resolve_all_sites()'s blind-spot check on a synthetic analysis: a caller with
      one declared field-load site and one undeclared site -- the declared one is
      exempted, the undeclared one is named as a blind spot (D1: a function with
      SOME declared classes gets no free pass for an undeclared one).
  (c) resolve_all_sites()'s argsites count check: 2 declared, 3 found in the
      disassembly -- FATAL (a count_mismatch entry naming the caller), not a
      silent re-use of the stale declaration.
  (d) frame_of()/an UNKNOWN frame on the deepest chain: a function the estimator
      could not classify must not be silently treated as a free 0 B frame; a
      `frame fn = BYTES` override in stack_edges.txt-shaped input makes it PASS
      (tagged "override") instead.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import stack_budget as sb  # noqa: E402

FAILURES = []


def check(name, cond, detail=""):
    if cond:
        print(f"  {name:60s} ok")
    else:
        print(f"  {name:60s} FAIL  {detail}")
        FAILURES.append(name)


# === (a) prologue estimator: no stale-register explosion ==============================

def fn_lines_epilogue_ldrpc_form():
    """push{r4-r7,lr}(20) + sub sp,#100(100) + ldr r3,[pc,#imm]=-476 + add sp,r3(476)
    = 596 B. Also an UNRELATED positive-looking literal load earlier in the function,
    immediately followed by an ordinary instruction -- must not linger."""
    return [
        " 1000:\tb5f0      \tpush\t{r4, r5, r6, r7, lr}",
        " 1002:\tb0e1      \tsub\tsp, #100",
        # unrelated literal load for some other purpose, then something that must
        # invalidate it (the OLD code never invalidated a `regs[reg]` entry at all)
        " 1004:\t4b03      \tldr\tr3, [pc, #16]\t@ (1014 <f+0x14>)",
        " 1006:\t1c18      \tadds\tr0, r3, #0",
        " 1008:\t4b02      \tldr\tr3, [pc, #12]\t@ (1018 <f+0x18>)",
        " 100a:\tb0fd      \tadd\tsp, r3",
        " 100c:\tbcf0      \tpop\t{r4, r5, r6, r7}",
        " 100e:\tbc01      \tpop\t{r0}",
        " 1010:\t4700      \tbx\tr0",
        " 1012:\t46c0      \tnop",
        # 0x00000030 -- an unrelated, small, POSITIVE literal (sign bit clear): the
        # first ldr above must not contribute anything even though the old code
        # would have looked it up too if a later add sp reused r3 without an
        # intervening ldr -- it doesn't reach that far here because of 1006/1008.
        " 1014:\t00000030 \t.word\t0x00000030",
        " 1018:\tfffffe24 \t.word\t0xfffffe24",   # -476
    ]


def fn_lines_epilogue_movslsls_form():
    """Same 596 B target via the SECOND legal Thumb-1 idiom
    (movs rN,#k; lsls rN,rN,#s; add sp,rN = k<<s), with a stale, WRONG-looking
    earlier ldr r3 (a huge value) that a naive reg-tracking dict would have no
    reason to distinguish from the real one -- must not affect the result at all,
    since this idiom never consults ldr_pending in the first place."""
    return [
        " 2000:\tb5f0      \tpush\t{r4, r5, r6, r7, lr}",
        " 2002:\tb0e1      \tsub\tsp, #100",
        " 2004:\t4b05      \tldr\tr3, [pc, #20]\t@ (201c <g+0x1c>)",
        " 2006:\t1c18      \tadds\tr0, r3, #0",       # invalidates the stale ldr
        " 2008:\t23ee      \tmovs\tr3, #238",
        " 200a:\t00db      \tlsls\tr3, r3, #1",
        " 200c:\tb0fd      \tadd\tsp, r3",
        " 200e:\tbcf0      \tpop\t{r4, r5, r6, r7}",
        " 2010:\tbc01      \tpop\t{r0}",
        " 2012:\t4700      \tbx\tr0",
        " 2014:\t46c0      \tnop",
        " 201c:\tdeadbeef  \t.word\t0xdeadbeef",       # the "unrelated garbage" 728 B trap
    ]


def test_a_estimator_no_explosion():
    fn_lines = {
        "epilogue_ldrpc_form": fn_lines_epilogue_ldrpc_form(),
        "epilogue_movslsls_form": fn_lines_epilogue_movslsls_form(),
    }
    est = sb.estimate_frames(fn_lines)
    check("(a) ldr-pc epilogue idiom = 596 B, not exploded",
          est["epilogue_ldrpc_form"] == {"bytes": 596, "unknown": False},
          est["epilogue_ldrpc_form"])
    check("(a) movs/lsls epilogue idiom = 596 B, stale ldr ignored",
          est["epilogue_movslsls_form"] == {"bytes": 596, "unknown": False},
          est["epilogue_movslsls_form"])


# === (b) per-site blind spot: one declared, one undeclared =============================

def test_b_blind_spot_per_site():
    # caller_a: two indirect sites through the SAME struct-field offset (20, declared)
    # -- both must be exempted, the whole-function-blanket bug this replaces would
    # have exempted them the same way, so this alone doesn't prove the fix.
    # caller_a also has a THIRD site at an undeclared offset (28) -- under the OLD
    # `caller -> impls` scheme this would have inherited the SAME free pass from the
    # offset-20 declaration; under the fix it must be named as a blind spot.
    analysis = {
        "indirect_sites": {
            "caller_a": [
                ("0x1000", "bl\t2000 <thunk1>", "r3"),   # -> ldr r3,[r4,#20]  (declared)
                ("0x1010", "bl\t2000 <thunk1>", "r3"),   # -> ldr r3,[r4,#20]  (declared)
                ("0x1020", "bl\t2000 <thunk1>", "r3"),   # -> ldr r3,[r4,#28]  (UNDECLARED)
            ],
        },
        "fn_insn_seq": {
            "caller_a": [
                (0x0ffa, "ldr\tr4, [sp, #4]"),
                (0x0ffc, "ldr\tr3, [r4, #20]"),
                (0x0ffe, "nop"),
                (0x100a, "ldr\tr4, [sp, #4]"),
                (0x100c, "ldr\tr3, [r4, #20]"),
                (0x100e, "nop"),
                (0x101a, "ldr\tr4, [sp, #4]"),
                (0x101c, "ldr\tr3, [r4, #28]"),
                (0x101e, "nop"),
            ],
        },
    }
    field_decls = {("Widget", "handler"): (20, {"impl_handler"})}
    argsite_decls = {}
    whole_func_decls = {}
    edges_to_add, blind, count_mismatches, legacy_ambiguous = sb.resolve_all_sites(
        analysis, field_decls, argsite_decls, whole_func_decls)
    check("(b) declared offset (20) exempted -> edge added",
          edges_to_add.get("caller_a") == {"impl_handler"}, edges_to_add)
    check("(b) no false positives on the declared sites",
          count_mismatches == [] and legacy_ambiguous == [])
    caller_blind = blind.get("caller_a", [])
    check("(b) exactly one blind site (the undeclared offset-28 one)",
          len(caller_blind) == 1, caller_blind)
    if caller_blind:
        addr, ins, detail = caller_blind[0]
        check("(b) the named blind site is the offset-28 one, not offset-20",
              addr == "0x1020" and "@28" in detail, caller_blind[0])


# === (b2) D1: a spilled base register is still a section anchor ========================

def test_b2_spill_is_not_redefinition():
    """The reviewer's synthetic reproduction: `ldr r3,[pc,#40]` materializes a
    section-anchor base, `str r3,[sp,#24]` SPILLS it (reads r3, does not redefine
    it), then `ldr r3,[r3,#20]` indexes off it -- byte-for-byte the same shape as a
    genuine `ldr r3,[r3,#20]` struct-field dereference through an instance pointer.
    Before the fix, _base_is_section_anchor()'s backward scan hit the `str` first
    and, because DEST_REG_RE also matches a str's first operand, treated it as a
    redefinition of r3 and stopped looking -- so it never saw the earlier
    `ldr r3,[pc,#40]` and returned False (not a section anchor), and the call site
    resolved to a false 'field' hit that could coincide with a declared field
    (e.g. BoxSource.records @20) and silently exempt an unrelated global/thunk
    dispatch. Must resolve to ('nonfield', None) -- a blind spot, not a field."""
    fn_insn_seq = {
        "spill_then_index": [
            (0x0f00, "ldr\tr3, [pc, #40]\t@ (0f2c <spill_then_index+0x2c>)"),
            (0x0f02, "str\tr3, [sp, #24]"),
            (0x0f04, "ldr\tr3, [r3, #20]"),
            (0x0f06, "nop"),
        ],
    }
    kind, off = sb.resolve_indirect_site(fn_insn_seq["spill_then_index"], 0x0f08, "r3")
    check("(b2) spilled section-anchor base resolves nonfield, not field @20",
          (kind, off) == ("nonfield", None), (kind, off))


# === (c) argsites count mismatch ========================================================

def test_c_argsites_count_mismatch():
    # 3 real sites in the disassembly, but the declaration only counted 2 -- must
    # FATAL naming the caller, not silently trust the stale count.
    analysis = {
        "indirect_sites": {
            "caller_b": [
                ("0x3000", "bl\t4000 <thunk2>", "r3"),
                ("0x3010", "bl\t4000 <thunk2>", "r3"),
                ("0x3020", "bl\t4000 <thunk2>", "r3"),
            ],
        },
        "fn_insn_seq": {
            # every site's dispatch register comes straight from an argument
            # register with no preceding write at all in this window -> 'nonfield'
            "caller_b": [
                (0x2ffc, "mov\tr0, r5"),
                (0x300a, "mov\tr0, r5"),
                (0x301a, "mov\tr0, r5"),
            ],
        },
    }
    field_decls = {}
    argsite_decls = {"caller_b": (2, {"impl_x", "impl_y"})}
    whole_func_decls = {}
    edges_to_add, blind, count_mismatches, legacy_ambiguous = sb.resolve_all_sites(
        analysis, field_decls, argsite_decls, whole_func_decls)
    check("(c) argsites mismatch reported (declared 2, found 3)",
          count_mismatches == [("caller_b", 2, 3)], count_mismatches)
    check("(c) no edge added on a mismatched declaration (don't trust it either)",
          "caller_b" not in edges_to_add, edges_to_add)


def test_c_argsites_count_match_is_clean():
    """Sanity check for (c): when the count DOES match, no mismatch and the impls
    are added as edges -- proves the check isn't just always failing."""
    analysis = {
        "indirect_sites": {
            "caller_c": [
                ("0x5000", "bl\t6000 <thunk3>", "r3"),
                ("0x5010", "bl\t6000 <thunk3>", "r3"),
            ],
        },
        "fn_insn_seq": {
            "caller_c": [
                (0x4ffc, "mov\tr0, r5"),
                (0x500a, "mov\tr0, r5"),
            ],
        },
    }
    argsite_decls = {"caller_c": (2, {"impl_z"})}
    edges_to_add, blind, count_mismatches, legacy_ambiguous = sb.resolve_all_sites(
        analysis, {}, argsite_decls, {})
    check("(c) matching argsites count -> no mismatch, edge added",
          count_mismatches == [] and edges_to_add.get("caller_c") == {"impl_z"})


# === (d) UNKNOWN frame on the deepest chain =============================================

def test_d_unknown_frame_fails_unless_overridden():
    # A tiny graph: root -> mid -> leaf, where `leaf` has NO .su entry and its
    # prologue could not be classified (the estimator's own "unaccountable
    # deallocation" case) -- frame_of() must report it as ("unknown", 0), and that
    # must be distinguishable from a legitimately-measured 0 B leaf.
    su_sizes = {"root": 100, "mid": 50}
    estimated = {"leaf": {"bytes": 0, "unknown": True}}
    edges = {"root": {"mid"}, "mid": {"leaf"}}

    b, src = sb.frame_of("leaf", su_sizes, estimated)
    check("(d) unresolvable leaf frame tagged 'unknown', not a trusted 0",
          (b, src) == (0, "unknown"), (b, src))

    total, path, cycles = sb.deepest_from("root", edges, su_sizes, estimated)
    on_chain_srcs = {name: s for name, _b, s in path}
    check("(d) the unknown frame is visible on the reported chain",
          on_chain_srcs.get("leaf") == "unknown", on_chain_srcs)

    # Same graph, but stack_edges.txt now carries `frame leaf = 40  (measured by
    # hand, 2026-09-10)` -- frame_of() must honor it instead of refusing.
    overrides = {"leaf": 40}
    b2, src2 = sb.frame_of("leaf", su_sizes, estimated, overrides)
    check("(d) an override makes the frame usable, tagged 'override'",
          (b2, src2) == (40, "override"), (b2, src2))
    total2, path2, cycles2 = sb.deepest_from("root", edges, su_sizes, estimated,
                                              overrides=overrides)
    check("(d) the overridden total is now fully determined (100+50+40=190)",
          total2 == 190, total2)


def test_d_load_extra_edges_parses_frame_override():
    """The `frame fn = BYTES  (freeform)` line load_extra_edges() itself must
    parse -- exercised separately from the frame_of() mechanics above."""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("# comment\n")
        f.write("frame leaf = 40  (measured by hand, 2026-09-10)\n")
        f.write("Widget.handler @20 -> impl_handler\n")
        f.write("caller_b argsites=2 -> impl_x impl_y\n")
        path = f.name
    try:
        field_decls, argsite_decls, whole_func_decls, frame_overrides = \
            sb.load_extra_edges(path)
        check("(d) frame override line parsed", frame_overrides == {"leaf": 40},
              frame_overrides)
        check("(d) field-offset line parsed alongside it",
              field_decls == {("Widget", "handler"): (20, {"impl_handler"})}, field_decls)
        check("(d) argsites line parsed alongside it",
              argsite_decls == {"caller_b": (2, {"impl_x", "impl_y"})}, argsite_decls)
    finally:
        os.unlink(path)


# === struct_field_offsets(): the D1 header-drift check, against the real header =========

def test_boxsource_offsets_match_real_header():
    """Not a fixture -- this reads the REAL source/pdna_box.h shipped in this repo,
    so it doubles as a regression test for the header-offset computation the guard
    trusts at build time. If this ever fails, either the header changed (update
    tools/stack_edges.txt) or struct_field_offsets() has a bug."""
    header_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "source", "pdna_box.h")
    if not os.path.exists(header_path):
        print("  (skip) source/pdna_box.h not found from this working directory")
        return
    with open(header_path) as f:
        offsets = sb.struct_field_offsets(f.read(), "BoxSource")
    expected = {
        "nboxes": 0, "last_box_is_party": 4, "start_box": 8, "is_bank": 12,
        "has_start": 13, "wp_count": 16, "records": 20, "menu_block": 24,
        "get_name": 28, "set_name": 32, "get_wp": 36, "set_wp": 40, "can_edit": 44,
        "commit": 48, "mark_dirty": 52, "note_add": 56, "note_box": 60, "capacity": 64,
    }
    check("BoxSource field offsets match the real header (natural ARM EABI layout)",
          offsets == expected,
          {k: v for k, v in offsets.items() if expected.get(k) != v})


def main():
    print("host_stack_budget_test.py")
    test_a_estimator_no_explosion()
    test_b_blind_spot_per_site()
    test_b2_spill_is_not_redefinition()
    test_c_argsites_count_mismatch()
    test_c_argsites_count_match_is_clean()
    test_d_unknown_frame_fails_unless_overridden()
    test_d_load_extra_edges_parses_frame_override()
    test_boxsource_offsets_match_real_header()
    print()
    if FAILURES:
        print(f"host_stack_budget_test: {len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("host_stack_budget_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
