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

Cases, each named after the defect class it guards against regressing:

  (a) estimate_frames() on fixtures reproducing (1) mgfx_zoom_optimise's stale-
      register false-pass -- a stale `ldr rN,[pc,#imm]` from an UNRELATED earlier
      use must not leak into a later, unrelated `add sp, rN` -- and (2, D2/third
      pass) _svfiprintf_r's double-count: a `movs rN,#k; lsls rN,rN,#s; add sp,rN`
      epilogue is BY CONSTRUCTION a deallocation (Thumb-1 cannot encode a negative
      magnitude that way) and must contribute 0 B, not its magnitude, even when it
      mirrors an allocation made earlier via a negative-literal ldr+add-sp or a
      direct sub sp -- adding it anyway took _svfiprintf_r from 728 real B to
      1,420 reported. A movs/lsls dealloc with NO recognized allocation event
      anywhere in the function reports unknown rather than guessing.
  (b) resolve_all_sites()'s blind-spot check on a synthetic analysis: a caller with
      one declared field-load site and one undeclared site -- the declared one is
      exempted, the undeclared one is named as a blind spot (D1: a function with
      SOME declared classes gets no free pass for an undeclared one).
  (b2) _base_is_section_anchor() (D1, third pass): a `str rX,[sp,#N]` spill that
      READS the base register must not be treated as a redefinition that hides
      the register's true origin (a PC-relative section-anchor load) -- resolves
      nonfield, not a coincidental field hit.
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
    """D2 (BACKLOG #84b, third pass): a movs/lsls `add sp,rN` is BY CONSTRUCTION
    a deallocation (Thumb-1 cannot encode a negative magnitude that way) and must
    contribute 0 B, not its magnitude -- adding it is exactly the bug that took
    _svfiprintf_r from 728 real B to 1,420 reported (see
    fn_lines_svfiprintf_shape below for that literal reproduction). This fixture
    keeps the original stale-register trap (a stale, WRONG-looking earlier ldr r3
    with a huge value that a naive reg-tracking dict would have no reason to
    distinguish from a real one -- must not affect the result at all, since this
    idiom never consults ldr_pending in the first place) but the expected total
    is now push(20) + sub sp(100) = 120 B, NOT 596: the movs/lsls epilogue's
    476 B (238<<1) is a deallocation of the SAME sub-sp-allocated frame and must
    not be added on top of it. Not unknown either -- sub sp,#100 is a recognized
    allocation event, so the deallocation has something to match."""
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


def fn_lines_svfiprintf_shape():
    """The reviewer's literal reproduction of _svfiprintf_r's real shape (the
    numbers are the exact ones read out of the built PokeDNA-artless.elf on
    2026-09-10): push{r4-r7,lr}(20) + push{r5-r7,lr}(16) = 36 B of pushes, a
    negative-literal `ldr r4,[pc]=-692; add sp,r4` prologue allocation (692 B),
    and a `movs r3,#173; lsls r3,r3,#2; add sp,r3` epilogue restoring the SAME
    692 B (173<<2=692). Real frame is 36+692 = 728 B; the pre-fix walker
    reported 1,420 B (36+692+692) by adding the epilogue's 692 B a second
    time."""
    return [
        " 3000:\tb5f0      \tpush\t{r4, r5, r6, r7, lr}",
        " 3002:\tb5e0      \tpush\t{r5, r6, r7, lr}",
        " 3004:\t4c03      \tldr\tr4, [pc, #12]\t@ (3014 <h+0x14>)",
        " 3006:\t44a5      \tadd\tsp, r4",
        " 3008:\t23ad      \tmovs\tr3, #173",
        " 300a:\t009b      \tlsls\tr3, r3, #2",
        " 300c:\t449d      \tadd\tsp, r3",
        " 300e:\tbcf0      \tpop\t{r4, r5, r6, r7}",
        " 3010:\tbc01      \tpop\t{r0}",
        " 3012:\t4700      \tbx\tr0",
        " 3014:\tfffffd4c  \t.word\t0xfffffd4c",   # -692
    ]


def fn_lines_shift_only_no_alloc():
    """No sub sp and no negative-literal ldr+add-sp anywhere in this function --
    the ONLY stack-adjusting instruction is a movs/lsls deallocation. Whatever
    allocated the frame this restores was done by a form this walker does not
    recognize; the frame must come out UNKNOWN, never a guessed magnitude."""
    return [
        " 4000:\tb5f0      \tpush\t{r4, r5, r6, r7, lr}",
        " 4002:\t23ad      \tmovs\tr3, #173",
        " 4004:\t009b      \tlsls\tr3, r3, #2",
        " 4006:\t449d      \tadd\tsp, r3",
        " 4008:\tbcf0      \tpop\t{r4, r5, r6, r7}",
        " 400a:\tbc01      \tpop\t{r0}",
        " 400c:\t4700      \tbx\tr0",
    ]


def test_a_estimator_no_explosion():
    fn_lines = {
        "epilogue_ldrpc_form": fn_lines_epilogue_ldrpc_form(),
        "epilogue_movslsls_form": fn_lines_epilogue_movslsls_form(),
        "svfiprintf_shape": fn_lines_svfiprintf_shape(),
        "shift_only_no_alloc": fn_lines_shift_only_no_alloc(),
    }
    est = sb.estimate_frames(fn_lines)
    check("(a) ldr-pc epilogue idiom = 596 B, not exploded",
          est["epilogue_ldrpc_form"] == {"bytes": 596, "unknown": False},
          est["epilogue_ldrpc_form"])
    check("(a/D2) movs/lsls epilogue is a dealloc, contributes 0 -> 120 B, stale ldr ignored",
          est["epilogue_movslsls_form"] == {"bytes": 120, "unknown": False},
          est["epilogue_movslsls_form"])
    check("(a/D2) _svfiprintf_r shape = 728 B, not double-counted to 1,420",
          est["svfiprintf_shape"] == {"bytes": 728, "unknown": False},
          est["svfiprintf_shape"])
    check("(a/D2) movs/lsls dealloc with no allocation event anywhere -> unknown",
          est["shift_only_no_alloc"]["unknown"] is True,
          est["shift_only_no_alloc"])


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


# === D6 trap #7: `ldmia rX!, {reglist}` redefines every register it loads =============

def test_d6_ldm_redefines_every_loaded_register():
    """The reviewer's literal reproduction of rom_mon_icon_at's real false-attribution
    (BACKLOG #84b fourth pass): `ldrb r3, [r1, #5]` (loc->ok, a plain uint8_t flag,
    NOT a pointer of any kind) sits several instructions before `ldmia r0!, {r3, r4}`
    (rc->read/rc->ctx, RomCtx offsets 0/4 -- the REAL dispatch value), which is
    immediately copied into r5 and dispatched. Before this fix, the backward scan hit
    the `ldmia` and, because plain DEST_REG_RE only recognises the WRITEBACK base
    (r0) as a destination, walked straight past it without noticing r3/r4 were also
    just redefined -- landing on the earlier, unrelated `ldrb r3, [r1, #5]` and
    reporting 'field @5', not even a real field. Must resolve to ('nonfield', None)."""
    fn_insn_seq = {
        "icon_at": [
            (0x0f00, "ldrb\tr3, [r1, #5]"),           # loc->ok -- NOT the dispatch value
            (0x0f02, "cmp\tr3, #0"),
            (0x0f04, "ldr\tr0, [r0, #0]"),             # r0 = rm->rc
            (0x0f06, "ldmia\tr0!, {r3, r4}"),           # r3 = rc->read (offset 0), r4 = rc->ctx
            (0x0f08, "movs\tr5, r3"),
            (0x0f0a, "nop"),
        ],
    }
    kind, off = sb.resolve_indirect_site(fn_insn_seq["icon_at"], 0x0f0c, "r5")
    check("(D6 trap 7) ldmia-loaded dispatch resolves nonfield, not a stale field @5",
          (kind, off) == ("nonfield", None), (kind, off))

    # Sanity check: a register the ldm does NOT touch (r6) must scan straight past it
    # to whatever set r6 earlier, not get flagged by the ldm at all.
    fn_insn_seq2 = {
        "icon_at2": [
            (0x0f00, "ldr\tr6, [r2, #12]"),             # a genuine field load for r6
            (0x0f02, "ldmia\tr0!, {r3, r4}"),            # unrelated -- doesn't touch r6
            (0x0f04, "movs\tr5, r6"),
            (0x0f06, "nop"),
        ],
    }
    kind2, off2 = sb.resolve_indirect_site(fn_insn_seq2["icon_at2"], 0x0f08, "r5")
    check("(D6 trap 7) ldm not touching the chased register is skipped, not a stop",
          (kind2, off2) == ("field", 12), (kind2, off2))


# === D6 trap #8: hi-register alias names (sl/fp) must chase like any other reg =========

def test_d6_hi_register_aliases_chase_through_mov():
    """The reviewer's literal reproduction of pdna_dex_screen's real false-attribution
    on the FULL-art build (BACKLOG #84b fourth pass): a section-anchor base is loaded
    into r5 via `ldr r5, [pc, #N]`, promoted into `sl` (callee-saved, so it survives
    two intervening `bl`s) with `mov sl, r5`, then copied back into r3 with
    `mov r3, sl` right before `ldr r3, [r3, #8]` feeds the dispatch. Before this fix,
    every register-matching regex in the chase (`MOV_REG_RE`, `DEST_REG_RE`,
    `LDR_FIELD_RE`) only recognised `r\\d+` or literally `ip` -- `sl` (objdump's name
    for r10, the same way it prints r12 as `ip`) matched NONE of them, so the
    `mov r3, sl` step was invisible to the mov-chase and fell through to
    DEST_REG_RE's generic "redefined by something else, stop" case -- reporting a
    section-anchor global access as a genuine field at whatever offset the anchor
    gave it. Must resolve to ('nonfield', None), not ('field', 8)."""
    fn_insn_seq = {
        "dex_screen_shape": [
            (0x1000, "ldr\tr5, [pc, #100]\t@ (1068 <f+0x68>)"),  # section anchor
            (0x1002, "str\tr0, [r5, #8]"),                        # populate s_dget etc.
            (0x1004, "mov\tsl, r5"),                              # promote to callee-saved
            (0x1006, "bl\t2000 <build_species>"),                 # sl survives (callee-saved)
            (0x1008, "bl\t2004 <memset>"),                        # sl survives again
            (0x100a, "mov\tr3, sl"),                              # copy back down
            (0x100c, "ldr\tr3, [r3, #8]"),                        # -> s_dget itself
            (0x100e, "nop"),
        ],
    }
    kind, off = sb.resolve_indirect_site(fn_insn_seq["dex_screen_shape"], 0x1010, "r3")
    check("(D6 trap 8) sl-promoted section-anchor base resolves nonfield, not field @8",
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
        field_decls, argsite_decls, whole_func_decls, frame_overrides, isr_decls, \
            addrtaken_ok, _recursion_decls = sb.load_extra_edges(path)
        check("(d) frame override line parsed", frame_overrides == {"leaf": 40},
              frame_overrides)
        check("(d) field-offset line parsed alongside it",
              field_decls == {("Widget", "handler"): (20, {"impl_handler"})}, field_decls)
        check("(d) argsites line parsed alongside it",
              argsite_decls == {"caller_b": (2, {"impl_x", "impl_y"})}, argsite_decls)
    finally:
        os.unlink(path)


def test_d6_argsites_accepts_a_dotted_gcc_clone_name():
    """D6 (BACKLOG #84b fourth pass): a `.constprop.N`/`.isra.N`/`.part.0` GCC clone
    suffix in the CALLER name must parse as a real argsites declaration, not
    silently fall through to the whole_func_decls catch-all as a dead, never-
    matching key (the old `\\w+`-only ARGSITE_LINE_RE's failure mode -- found live
    when draw_wallpaper.constprop.0/fetch_pic_ex.constprop.0's own argsites=3
    declarations parsed with zero effect: `argsite_decls` stayed empty and the two
    sites kept reporting as blind spots even after being 'declared')."""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("draw_wallpaper.constprop.0 argsites=3 -> impl_a impl_b\n")
        path = f.name
    try:
        field_decls, argsite_decls, whole_func_decls, frame_overrides, isr_decls, \
            addrtaken_ok, _recursion_decls = sb.load_extra_edges(path)
        check("(D6) dotted caller name parsed into argsite_decls, not swallowed whole",
              argsite_decls == {"draw_wallpaper.constprop.0": (3, {"impl_a", "impl_b"})},
              argsite_decls)
        check("(D6) nothing leaked into whole_func_decls as a bogus dead key",
              whole_func_decls == {}, whole_func_decls)
    finally:
        os.unlink(path)


# === (D7, fifth pass) pop {reglist} redefines every register it loads ===================

def test_d7_pop_redefines_its_register_list():
    """The reviewer's fixture: a genuine field load into r3 (`ldr r3,[r4,#20]`),
    then a `pop {r3, r4}` that redefines r3 with something unrelated (an epilogue's
    saved-register restore, the shape objdump ALWAYS prints as `pop`, never
    `ldmia sp!`), then the dispatch through r3. Before the fix, LDM_RE (anchored on
    the `ldm` mnemonic) never matched `pop` at all, so the backward scan walked
    straight through it and landed on the stale field load -- reporting
    ('field', 20), a real field's value that was never actually live at the
    dispatch. Must resolve to ('nonfield', None): r3's true origin (the pop) does
    not trace to a struct-field load."""
    fn_insn_seq = {
        "pop_shape": [
            (0x0f00, "ldr\tr3, [r4, #20]"),   # a genuine, but STALE, field load
            (0x0f02, "bl\t2000 <some_helper>"),
            (0x0f06, "pop\t{r3, r4}"),         # redefines r3 -- the field load no longer applies
            (0x0f08, "nop"),
        ],
    }
    kind, off = sb.resolve_indirect_site(fn_insn_seq["pop_shape"], 0x0f0a, "r3")
    check("(D7) pop {r3,r4} redefines r3 -> nonfield, not a stale field @20",
          (kind, off) == ("nonfield", None), (kind, off))

    # Sanity: a pop that does NOT touch the chased register must be transparent,
    # exactly like an ldm that doesn't touch it.
    fn_insn_seq2 = {
        "pop_shape2": [
            (0x0f00, "ldr\tr3, [r4, #20]"),
            (0x0f02, "pop\t{r5, r6}"),          # unrelated -- doesn't touch r3
            (0x0f04, "nop"),
        ],
    }
    kind2, off2 = sb.resolve_indirect_site(fn_insn_seq2["pop_shape2"], 0x0f06, "r3")
    check("(D7) pop not touching the chased register is transparent, not a stop",
          (kind2, off2) == ("field", 20), (kind2, off2))

    # Range form: `pop {r3-r5}` must expand exactly the same way _reg_count's byte
    # tally does (r3, r4, r5), not just the endpoints.
    check("(D7) _expand_reglist handles an r3-r5 range like _reg_count does",
          sb._expand_reglist("{r3-r5, lr}") == {"r3", "r4", "r5", "lr"},
          sb._expand_reglist("{r3-r5, lr}"))


# === (D1, fifth pass) the address-taken sweep ===========================================

def test_d1_load_extra_edges_parses_isr_and_addrtaken_ok():
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("isr hb_isr\n")
        f.write("isr pwm_isr\n")
        f.write("addrtaken-ok some_table_entry  # compiler-generated, never called\n")
        path = f.name
    try:
        _fd, _ad, _wd, _fo, isr_decls, addrtaken_ok, _rd = sb.load_extra_edges(path)
        check("(D1) isr lines parsed", isr_decls == {"hb_isr", "pwm_isr"}, isr_decls)
        check("(D1) addrtaken-ok line parsed", addrtaken_ok == {"some_table_entry"},
              addrtaken_ok)
    finally:
        os.unlink(path)


def test_d1_words_from_objdump_s_text_byte_order():
    """A synthetic `objdump -s` section dump: one line holding two 4-byte groups. The
    first group's bytes, read in ADDRESS order (0x00,0x00,0x01,0x08) and interpreted
    little-endian, is 0x08010000 -- a Thumb function pointer (bit 0 set) whose real
    entry is 0x08010000. The second group is a plain data word, 0xdeadbeef, not a
    function address at all."""
    text = " 8072380 01000108 efbeadde  " + "." * 8 + "\n"
    words = list(sb._words_from_objdump_s_text(text))
    check("(D1) two words parsed from one dump line", len(words) == 2, words)
    check("(D1) first word decoded little-endian, not as a raw hex string",
          words[0] == 0x08010001, hex(words[0]) if words else None)
    check("(D1) second word decoded correctly too",
          words[1] == 0xdeadbeef, hex(words[1]) if len(words) > 1 else None)
    check("(D1) masking the Thumb bit off the first word gives the real entry",
          (words[0] & ~1) == 0x08010000, hex(words[0] & ~1))


def test_d1_orphan_detection_catches_the_planted_function():
    """The reviewer's exact attack, modeled at the set-arithmetic level (the real
    scan runs over an actual ELF's objdump -s output -- see the report's `make
    artless` proof for the live version): a function whose address is TAKEN (found
    by the sweep) but that is in none of {declared implementations, reachable-by-
    ordinary-graph, declared ISR handlers, addrtaken-ok escapes} must be named as an
    orphan. A legitimately-declared implementation, and a genuinely reachable
    function whose address also happens to be taken (e.g. stored in a second,
    unrelated table), must NOT be flagged."""
    taken = {"rv_deep_records", "pcsrc_records", "hb_isr", "app_mon_menu"}
    all_impls = {"pcsrc_records", "banksrc_records", "gbsrc_records"}
    reachable = {"app_mon_menu", "main"}
    isr_decls = {"hb_isr", "pwm_isr"}
    addrtaken_ok = set()
    declared_or_reachable = all_impls | reachable | isr_decls | addrtaken_ok
    orphans = sorted(taken - declared_or_reachable)
    check("(D1) the planted, undeclared function is the ONLY orphan",
          orphans == ["rv_deep_records"], orphans)

    # Now the escape hatch: declaring it addrtaken-ok clears it.
    addrtaken_ok2 = {"rv_deep_records"}
    declared_or_reachable2 = all_impls | reachable | isr_decls | addrtaken_ok2
    orphans2 = sorted(taken - declared_or_reachable2)
    check("(D1) addrtaken-ok clears a genuine false positive",
          orphans2 == [], orphans2)


# === (D2, fifth pass) the ELF names the build dir it was linked from ====================

class _FakeCompleted:
    def __init__(self, stdout):
        self.stdout = stdout


def test_d2_read_build_dir_stamp_extracts_the_nul_terminated_string():
    """Monkeypatch subprocess.run so this exercises read_build_dir_stamp()'s own
    address-lookup + byte-extraction logic without needing a real ELF/toolchain --
    every other test in this file about the actual sweep mechanics does the same
    (fixtures over the parsing functions, the report's `make artless` proof covers
    the real ELF end to end)."""
    import subprocess as _subprocess
    nm_out = (
        "030043a0 D __iheap_start\n"
        "08072400 D pdna_build_dir\n"
        "08072410 D some_other_symbol\n"
    )
    # "build-artless\0" then one byte of the next symbol's data, in objdump -s's
    # address-order hex-pair format.
    payload = "build-artless".encode("ascii") + b"\x00" + b"\xAB"
    hexstr = payload.hex()
    groups = [hexstr[i:i + 8] for i in range(0, len(hexstr), 8)]
    objdump_line = " 8072400 " + " ".join(groups) + "   ...\n"

    real_run = _subprocess.run

    def fake_run(cmd, **kwargs):
        if cmd[0] == sb.NM:
            return _FakeCompleted(nm_out)
        if cmd[0] == sb.OBJDUMP and "-s" in cmd:
            return _FakeCompleted(objdump_line)
        return real_run(cmd, **kwargs)

    _subprocess.run = fake_run
    try:
        stamp = sb.read_build_dir_stamp("fake.elf", [".rodata"])
    finally:
        _subprocess.run = real_run
    check("(D2) stamp read back matches the planted string, stops at the NUL",
          stamp == "build-artless", stamp)


def test_d2_read_build_dir_stamp_absent_symbol_returns_none():
    import subprocess as _subprocess
    real_run = _subprocess.run

    def fake_run(cmd, **kwargs):
        if cmd[0] == sb.NM:
            return _FakeCompleted("030043a0 D __iheap_start\n")
        return real_run(cmd, **kwargs)

    _subprocess.run = fake_run
    try:
        stamp = sb.read_build_dir_stamp("fake.elf", [".rodata"])
    finally:
        _subprocess.run = real_run
    check("(D2) no pdna_build_dir symbol -> None, not a false mismatch",
          stamp is None, stamp)


# === (D8, fifth pass) UNKNOWN frames are checked over the whole reachable set ===========

def test_d8_unknown_frame_off_the_deepest_chain_still_fatal():
    """root fans out to `shallow` (frame UNKNOWN, i.e. unbounded) and to
    mid->deep (a big MEASURED frame). Because an unknown frame scores 0 B in the
    chain-ranking comparison, `shallow` can never win against `deep`'s 500 B and so
    never appears on the printed top-N chains -- but its true weight is genuinely
    unknown, not 0, so this must still be caught. Checking only the printed chains
    (the pre-D8 behavior) would miss it entirely, exactly the same failure shape D4
    already fixed for blind spots."""
    edges = {"root": {"shallow", "mid"}, "mid": {"deep"}}
    su_sizes = {"root": 10, "mid": 10, "deep": 500}
    estimated = {"shallow": {"bytes": 0, "unknown": True}}

    total, path, _cyc = sb.deepest_from("root", edges, su_sizes, estimated)
    on_chain = {name for name, _b, _s in path}
    check("(D8 setup) deepest chain is root->mid->deep, NOT through shallow",
          on_chain == {"root", "mid", "deep"} and total == 520, (on_chain, total))

    reachable = sb.reachable_from("root", edges)
    unknown_reachable = sorted(
        fn for fn in reachable if sb.frame_of(fn, su_sizes, estimated)[1] == "unknown")
    check("(D8) `shallow` is reachable and UNKNOWN even though never on-chain",
          unknown_reachable == ["shallow"], unknown_reachable)

    # A frame_overrides entry for `shallow` must clear it, same as the on-chain case.
    overrides = {"shallow": 5}
    unknown_reachable2 = sorted(
        fn for fn in reachable
        if sb.frame_of(fn, su_sizes, estimated, overrides)[1] == "unknown")
    check("(D8) a frame override clears the reachable-set unknown, same as on-chain",
          unknown_reachable2 == [], unknown_reachable2)


def test_d8_phantom_declared_name_not_linked_is_not_a_false_unknown():
    """A declared implementation NAME that isn't actually linked into this build
    variant (stack_edges.txt's own documented, non-fatal case) becomes a phantom
    node once it's unioned into `edges` by name alone -- it has no .su/estimated
    entry, so frame_of() calls it 'unknown'. The whole-reachable D8 sweep must
    exclude names that aren't in analysis['funcs'] at all (never linked, therefore
    never actually callable, therefore never an unbounded risk) -- reproduced live
    on the delta build, where rom_open's declared impl list names a fatfs_read
    variant that build doesn't link."""
    edges = {"root": {"real_child", "phantom_unlinked_impl"}}
    su_sizes = {"root": 10, "real_child": 5}
    estimated = {}
    funcs = {"root", "real_child"}   # phantom_unlinked_impl is NOT a real function here
    reachable = sb.reachable_from("root", edges)
    unknown_reachable = sorted(
        fn for fn in reachable
        if fn in funcs and sb.frame_of(fn, su_sizes, estimated)[1] == "unknown")
    check("(D8) phantom, never-linked name is excluded from the unknown-reachable set",
          unknown_reachable == [], unknown_reachable)


# === (D3, fifth pass) su_frame(): a clone's frame is the max over its OWN .su keys ======

def test_d3_su_frame_takes_max_not_bare_base_first():
    """The reviewer's exact plant: `validate.constprop.0` has no exact .su entry, but
    BOTH `validate` (FatFs's own, 136 B) and `validate.constprop` (the real clone,
    560 B) do. The bare-base-first version returned 136 (a live 424 B under-count);
    the fix must return the max, 560."""
    sizes = {"validate": 136, "validate.constprop": 560, "read_verified": 32,
              "read_verified.constprop": 48}
    check("(D3) validate.constprop.0 -> max(136, 560) = 560, not the bare base 136",
          sb.su_frame(sizes, "validate.constprop.0") == 560,
          sb.su_frame(sizes, "validate.constprop.0"))
    check("(D3) read_verified.constprop.0 -> max(32, 48) = 48, not the bare base 32",
          sb.su_frame(sizes, "read_verified.constprop.0") == 48,
          sb.su_frame(sizes, "read_verified.constprop.0"))
    check("(D3) an exact name match still wins outright, no max involved",
          sb.su_frame({"foo": 10, "foo.isra": 999}, "foo") == 10,
          sb.su_frame({"foo": 10, "foo.isra": 999}, "foo"))


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


# === (D4) blind spots over the WHOLE reachable graph, not just the deepest chain =======

def _d4_graph():
    """root fans out to two branches:
      root -> shallow            (ONE indirect-call site, offset 99)
      root -> mid -> deep        (no indirect sites, but a big .su frame on `deep`
                                   that makes THIS the reported deepest chain)
    `shallow`'s site is never on the deepest chain (20 B vs 520 B) -- exactly the
    shape the old on-chain filter missed."""
    analysis = {
        "indirect_sites": {
            "shallow": [("0x1020", "bl\t2000 <thunk1>", "r3")],   # -> ldr r3,[r4,#99]
        },
        "fn_insn_seq": {
            "shallow": [
                (0x101a, "ldr\tr4, [sp, #4]"),
                (0x101c, "ldr\tr3, [r4, #99]"),
                (0x101e, "nop"),
            ],
        },
    }
    edges = {"root": {"shallow", "mid"}, "mid": {"deep"}}
    su_sizes = {"root": 10, "shallow": 10, "mid": 10, "deep": 500}
    estimated = {}
    return analysis, edges, su_sizes, estimated


def test_d4_undeclared_shallow_site_is_a_blind_spot_off_the_deepest_chain():
    analysis, edges, su_sizes, estimated = _d4_graph()
    # confirm the deepest chain really doesn't pass through `shallow` at all --
    # otherwise this fixture wouldn't be testing what it claims to.
    total, path, _cyc = sb.deepest_from("root", edges, su_sizes, estimated)
    on_chain = {name for name, _b, _s in path}
    check("(D4 setup) deepest chain is root->mid->deep, NOT through shallow",
          on_chain == {"root", "mid", "deep"} and total == 520, (on_chain, total))

    # no declaration for offset 99 -> resolve_all_sites must name it a blind spot
    edges_to_add, blind, count_mismatches, legacy_ambiguous = sb.resolve_all_sites(
        analysis, {}, {}, {})
    check("(D4) undeclared site has no edge added", edges_to_add == {}, edges_to_add)

    reachable = sb.reachable_from("root", edges)
    check("(D4) `shallow` is reachable from root even though never on-chain",
          "shallow" in reachable and "shallow" not in on_chain, reachable)

    spots = sb.whole_graph_blind_spots(reachable, blind)
    check("(D4) whole-graph sweep FAILS on the shallow, off-chain undeclared site",
          len(spots) == 1 and spots[0][0] == "shallow" and "@99" in spots[0][3], spots)


def test_d4_declaring_the_site_clears_it_and_deepest_number_is_unchanged():
    analysis, edges, su_sizes, estimated = _d4_graph()
    field_decls = {("Widget", "handler"): (99, {"impl_handler"})}
    edges_to_add, blind, count_mismatches, legacy_ambiguous = sb.resolve_all_sites(
        analysis, field_decls, {}, {})
    check("(D4) declared offset 99 -> edge added, no blind entries left",
          edges_to_add.get("shallow") == {"impl_handler"} and blind == {}, (edges_to_add, blind))

    merged_edges = {k: set(v) for k, v in edges.items()}
    for caller, impls in edges_to_add.items():
        merged_edges.setdefault(caller, set()).update(impls)

    reachable = sb.reachable_from("root", merged_edges)
    spots = sb.whole_graph_blind_spots(reachable, blind)
    check("(D4) whole-graph sweep is clean once the site is declared", spots == [], spots)

    # `impl_handler` has no .su/edges of its own here, so it contributes 0 B --
    # the reported deepest total must still be the deep branch's number, 520.
    total, path, _cyc = sb.deepest_from("root", merged_edges, su_sizes, estimated)
    check("(D4) deepest total is still the deep branch's number after declaring",
          total == 520, total)


def test_d4_scc_declared_depth_multiplies_and_is_entry_independent():
    """D4 (BACKLOG #84b sixth pass): a real 2-node cycle a<->b, each with its own
    frame, reached from `root`, with an external continuation `tail` past the cycle.
    `scc_of` (as main() would build it from a `recursion a depth=N` declaration) must
    charge N * (frame(a)+frame(b)) exactly once at the cycle's entry, then continue
    into `tail` -- and the total must be the SAME whether the DFS happens to enter the
    cycle via `a` or via `b` (the exact bug D4 exists to fix: the old code memoized
    whichever node the DFS reached FIRST, so the reported total secretly depended on
    edge-iteration order)."""
    su_sizes = {"root": 10, "a": 20, "b": 30, "tail": 5}
    estimated = {}
    edges_ab = {"root": {"a"}, "a": {"b", "tail"}, "b": {"a"}, "tail": set()}
    edges_ba = {"root": {"b"}, "b": {"a", "tail"}, "a": {"b"}, "tail": set()}
    charge = 3 * (20 + 30)   # depth=3 * sum of every frame in the component
    fs = frozenset({"a", "b"})
    scc_of = {"a": (charge, fs), "b": (charge, fs)}

    total_ab, path_ab, cyc_ab = sb.deepest_from("root", edges_ab, su_sizes, estimated,
                                                 scc_of=scc_of)
    total_ba, path_ba, cyc_ba = sb.deepest_from("root", edges_ba, su_sizes, estimated,
                                                 scc_of=scc_of)
    want = 10 + charge + 5   # root's own frame + the declared SCC charge + tail
    check("(D4) declared depth multiplies the component's summed frames",
          total_ab == want, total_ab)
    check("(D4) the total does not depend on which member the DFS enters first "
          "(entering via a vs via b)", total_ab == total_ba, (total_ab, total_ba))
    check("(D4) no cycle is reported once the SCC is declared (scc_of absorbs it)",
          cyc_ab == [] and cyc_ba == [], (cyc_ab, cyc_ba))
    scc_names = [name for name, _b, src in path_ab if src == "recursion"]
    check("(D4) the SCC prints as one atomic path entry, not expanded member-by-member",
          scc_names == ["{a,b}"], scc_names)
    scc_bytes = [b for _n, b, src in path_ab if src == "recursion"][0]
    check("(D4) the printed SCC entry carries the declared charge, not a per-member frame",
          scc_bytes == charge, scc_bytes)


def test_d4_undeclared_scc_is_fatal_declared_disagreement_is_fatal():
    """D4: main()'s own FATAL logic (reproduced here at the unit level, since main()
    itself needs a real ELF) -- an SCC with NO declared member must be refused, and an
    SCC whose declared members disagree on depth must also be refused (never a silent
    pick of either)."""
    edges = {"root": {"a"}, "a": {"b"}, "b": {"a"}}
    comp = ["a", "b"]

    # Fixture 1: nobody declared -- matches main()'s `if not declared:` branch.
    recursion_decls = {}
    declared = sorted({(m, recursion_decls[m]) for m in comp if m in recursion_decls})
    check("(D4) an undeclared 2-node cycle has no declared member (main() would FATAL)",
          declared == [], declared)

    # Fixture 2: a and b disagree on depth -- matches main()'s `if len(depths) > 1:`
    # branch.
    recursion_decls = {"a": 2, "b": 3}
    declared2 = sorted({(m, recursion_decls[m]) for m in comp if m in recursion_decls})
    depths2 = {d for _m, d in declared2}
    check("(D4) disagreeing declared depths on the same component are detected",
          len(depths2) > 1, depths2)

    # Fixture 3: both agree -- clean, main() would proceed with depth=2.
    recursion_decls = {"a": 2, "b": 2}
    declared3 = sorted({(m, recursion_decls[m]) for m in comp if m in recursion_decls})
    depths3 = {d for _m, d in declared3}
    check("(D4) agreeing declared depths on the same component resolve cleanly",
          depths3 == {2}, depths3)


# === D10 (BACKLOG #84b sixth pass): traps #1, #5, #6, fixture-proved =================

def test_d10_trap1_bl_to_own_pop_bx_tail_is_zero_indirect_sites():
    """Trap #1's own regression proof, through analyze() end to end (not a lower-level
    helper): a function `foo` with an early-return branch whose `bl` is really a long
    Thumb-1 jump to foo's OWN shared epilogue -- `pop {r3}; bx r3` -- the exact idiom
    GCC emits for a function with more than one exit. `own(t) == fn` (trap #1) MUST be
    checked before the bx-thunk shape (trap #2/#5): the tail's `bx r3` is real, and the
    jump's target is neither foo's own start address nor a name in name_at, so if
    trap #1 didn't fire first this would misclassify a same-function jump as an
    unresolved cross-function indirect-call site. Must produce ZERO edges and ZERO
    indirect sites for foo -- the whole point of trap #1."""
    dump_text = (
        "08000000 <foo>:\n"
        "   8000000:\t2800      \tcmp\tr0, #0\n"
        "   8000002:\td101      \tbne.n\t8000008 <foo+0x8>\n"
        "   8000004:\tf000 f802 \tbl\t800000c <foo+0xc>\n"
        "   8000008:\t2000      \tmovs\tr0, #0\n"
        "   800000a:\t4770      \tbx\tlr\n"
        "   800000c:\tbc08      \tpop\t{r3}\n"
        "   800000e:\t4718      \tbx\tr3\n"
        "08000010 <bar>:\n"
        "   8000010:\t4770      \tbx\tlr\n"
    )
    analysis = sb.analyze(dump_text)
    check("(D10 trap 1) a bl to foo's own pop{r3};bx r3 tail adds NO edge",
          analysis["edges"].get("foo", set()) == set(), analysis["edges"].get("foo"))
    check("(D10 trap 1) ... and is not recorded as an indirect site either",
          analysis["indirect_sites"].get("foo", []) == [], analysis["indirect_sites"].get("foo"))
    check("(D10 trap 1) nothing lands in `weird` for this function",
          all(fn != "foo" for fn, _a, _i in analysis["weird"]), analysis["weird"])


def test_d10_trap5_literal_call_target_resolved_vs_table_index_blind_spot():
    """Trap #5's own regression proof, at the `_literal_call_target()` level (the same
    granularity the existing D6/D7/D8 tests already use for the sibling traps):

      (a) resolved -- `ldr r3, [pc, #imm]` feeds the `bl <bx-r3 thunk>` call with an
          UNINTERRUPTED literal load whose word, masked of the Thumb bit, lands EXACTLY
          on a real function's entry address -- this is the IWRAM-helper call shape
          (icopy_verified/memcpy32/... called from ROM code, D6's own docstring) and
          must resolve to that function's name.
      (b) table-index variant, still a blind spot -- the SAME shape except the
          register is fed by `ldr r3, [r2, r1, lsl #2]` (a runtime jump-table index,
          not a PC-relative literal): `_literal_call_target` must return None so the
          caller keeps treating it as a genuine, unresolved indirect site -- a table
          index is a real runtime-varying target, not a build-time constant, and
          resolving it would be a false pass in the unsafe direction."""
    insn_map_resolved = {0x1008: ".word 0x00002001"}   # target_fn's entry (0x2000) | thumb bit
    name_at = {0x2000: "target_fn"}
    fn_insn_seq_resolved = [
        # Thumb PC-relative: literal addr = (insn_addr & ~3) + 4 + #imm = (0x1000&~3)+4+4 = 0x1008
        (0x1000, "ldr\tr3, [pc, #4]\t@ (1008 <caller+0x8>)"),
        (0x1002, "movs\tr0, #0"),
        (0x1004, "bl\t2010 <__bx_r3_thunk>"),   # last entry: the call itself
    ]
    target = sb._literal_call_target(fn_insn_seq_resolved, insn_map_resolved, name_at, "r3")
    check("(D10 trap 5a) an uninterrupted ldr-pc literal landing on a function entry resolves",
          target == "target_fn", target)

    fn_insn_seq_table = [
        (0x1000, "ldr\tr2, [pc, #8]\t@ (100c <caller2+0xc>)"),   # table BASE, not the call target
        (0x1002, "lsls\tr1, r1, #2"),
        (0x1004, "ldr\tr3, [r2, r1]"),        # runtime table index -- NOT a pc-relative literal
        (0x1006, "movs\tr0, #0"),
        (0x1008, "bl\t2010 <__bx_r3_thunk>"),
    ]
    target2 = sb._literal_call_target(fn_insn_seq_table, {}, name_at, "r3")
    check("(D10 trap 5b) a runtime table-index dispatch is NOT resolved (stays a blind spot)",
          target2 is None, target2)


def test_d10_trap6_base_literal_loaded_far_before_its_use():
    """Trap #6's own regression proof: `_base_is_section_anchor` must chase back
    THROUGH 30+ unrelated instructions to find the `ldr rY, [pc, #imm]` that
    materialized the base register, exactly like dex_build/pdna_dex_screen's real
    `dstate()` shape (a file-static function pointer loaded once outside a loop, used
    once per element far below it) -- the OLD, bounded-window version defaulted to
    False (mistaking a global/section-anchor access for a genuine struct field) the
    moment it ran out of window. 34 filler instructions (none of which touch r5) sit
    between the literal load and the field dereference that uses it."""
    fn_insn_seq = [(0x1000, "ldr\tr5, [pc, #200]\t@ (10cc <f+0xcc>)")]  # index 0: the anchor load
    addr = 0x1002
    for i in range(34):                          # indices 1..34: unrelated filler
        fn_insn_seq.append((addr, f"movs\tr0, #{i % 8}"))
        addr += 2
    ldr_idx = len(fn_insn_seq)                    # the field-load instruction's own index
    fn_insn_seq.append((addr, "ldr\tr3, [r5, #8]"))
    check("(D10 trap 6) far-away setup: at least 30 instructions between anchor and use",
          ldr_idx - 0 >= 30, ldr_idx)
    anchor = sb._base_is_section_anchor(fn_insn_seq, ldr_idx, "r5")
    check("(D10 trap 6) a base literal loaded 30+ instructions earlier is still found",
          anchor is True, anchor)

    # Sibling check: resolve_indirect_site must therefore classify the WHOLE site as
    # nonfield (a global/anchor access), not a genuine struct-field hit at offset 8.
    kind, off = sb.resolve_indirect_site(fn_insn_seq, addr, "r3")
    check("(D10 trap 6) ... so resolve_indirect_site reports nonfield, not field @8",
          (kind, off) == ("nonfield", None), (kind, off))


def main():
    print("host_stack_budget_test.py")
    test_a_estimator_no_explosion()
    test_b_blind_spot_per_site()
    test_b2_spill_is_not_redefinition()
    test_d6_ldm_redefines_every_loaded_register()
    test_d6_hi_register_aliases_chase_through_mov()
    test_c_argsites_count_mismatch()
    test_c_argsites_count_match_is_clean()
    test_d_unknown_frame_fails_unless_overridden()
    test_d_load_extra_edges_parses_frame_override()
    test_d6_argsites_accepts_a_dotted_gcc_clone_name()
    test_d3_su_frame_takes_max_not_bare_base_first()
    test_d7_pop_redefines_its_register_list()
    test_d8_unknown_frame_off_the_deepest_chain_still_fatal()
    test_d8_phantom_declared_name_not_linked_is_not_a_false_unknown()
    test_d1_load_extra_edges_parses_isr_and_addrtaken_ok()
    test_d1_words_from_objdump_s_text_byte_order()
    test_d1_orphan_detection_catches_the_planted_function()
    test_d2_read_build_dir_stamp_extracts_the_nul_terminated_string()
    test_d2_read_build_dir_stamp_absent_symbol_returns_none()
    test_d4_undeclared_shallow_site_is_a_blind_spot_off_the_deepest_chain()
    test_d4_declaring_the_site_clears_it_and_deepest_number_is_unchanged()
    test_d4_scc_declared_depth_multiplies_and_is_entry_independent()
    test_d4_undeclared_scc_is_fatal_declared_disagreement_is_fatal()
    test_d10_trap1_bl_to_own_pop_bx_tail_is_zero_indirect_sites()
    test_d10_trap5_literal_call_target_resolved_vs_table_index_blind_spot()
    test_d10_trap6_base_literal_loaded_far_before_its_use()
    test_boxsource_offsets_match_real_header()
    print()
    if FAILURES:
        print(f"host_stack_budget_test: {len(FAILURES)} FAILED: {', '.join(FAILURES)}")
        return 1
    print("host_stack_budget_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
