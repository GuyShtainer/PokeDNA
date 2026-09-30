#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_y12_togame_gb_sites_test.py -- structural pins for BACKLOG #286 (lane y12-286): TO GAME on a Bank cell during
a Bank visit from a RESIDENT Game Boy session runs THE DROP'S OWN DOWN ARM and lands what a drop lands.
Pure text checks against the shipped source (no build), same posture as host_y10_togame_sites_test.py (whose helpers
it imports). Every fact function is shared by the real check and by an in-memory MUTATION that must turn it red.

  G1. the row: both occupied-cell branches of app_mon_menu (native + Gen-3 cell) offer TO GAME through
      xg_togame_gb_row(is_bank, app_gen3_pc_live(), togame_gb_writable()); togame_gb_writable() is exactly
      `pdna_gen12_resident() && app_can_edit()` (never the read-only mount, never an Everdrive); the pure predicate
      is `is_bank && !pc_live && gb_writable` (mutually exclusive with xg_togame_row by construction).
  G2. the action: A_TOGAME sets the one-shot request for `native || <the gb row>` -- and only then falls to
      app_inject_to_game (a Gen-3 cell in a GB session must never reach the Gen-3 injector).
  G3. the consumer: togame_native_run routes on xg_togame_gb_row(true, app_gen3_pc_live(), accept_down-present) to
      bank_togame_gb, else to app_bank_togame_native, inside the boxoam bracket, after the off-Bank refusal.
  G4. bank_togame_gb: gates and destination pick BEFORE the arms; a native cell runs bank_down_dispatch (never an arm's
      internals); the bridge consume (bank_down_consume, arm != EXACT) comes AFTER the dispatch; a Gen-3 cell runs
      bank_down_g3_run; nothing is deferred or written to the Gen-3 save; the SENT TO GAME message follows a fresh
      boxoam_suspend (y10 H4).
  G5. MENU-vs-DROP PARITY: bank_down_dispatch is called with the same 8-argument shape by drop_held and bank_togame_gb
      (origin = the carry statics / the cell's own coordinates); bank_down_g3_run is called by BOTH drop_held_down_g3
      and bank_togame_gb with the same shape; the arm is derived by the same xg_bank_down_arm expression on both sides.
  G6. the destination pick (gb_togame_pick_box): skips the party, requires a writable trusted box with room, and
      reports the append slot -- the same predicates gb_accept_down_hook's own pre-flight applies.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_y10_togame_sites_test as y10  # noqa: E402  (helpers only; its main() is guarded)

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "source" / "pdna_main.c"
BOX = ROOT / "source" / "pdna_box.c"
GEN12 = ROOT / "source" / "pdna_gen12.c"
GATE = ROOT / "source" / "xfer_gate.c"

strip_comments, function_body, first, ordered = y10.strip_comments, y10.function_body, y10.first, y10.ordered
call_args, mutate = y10.call_args, y10.mutate

checks = 0
fails: list[str] = []


def check(cond: bool, msg: str) -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


GB_ROW_CALL = r"xg_togame_gb_row\(\s*is_bank\s*,\s*app_gen3_pc_live\(\)\s*,\s*togame_gb_writable\(\)\s*\)"


# ---- G1 --------------------------------------------------------------------------------------
def rows_facts(menu: str) -> tuple[bool, str]:
    m = strip_comments(menu)
    nat_i, occ_i = m.find("if (native) {"), m.find("} else if (occupied) {")
    emp_i = m.rfind("} else {", 0, m.find("PDNA_LBL_CANCEL"))   # the empty-slot branch (the occupied one holds inner `} else {`s)
    if nat_i < 0 or occ_i < 0 or emp_i < 0:
        return False, "the native / occupied / empty branches of app_mon_menu were not found"
    nat, occ = m[nat_i:occ_i], m[occ_i:emp_i]
    pat = r"if\s*\(\s*" + GB_ROW_CALL + r"\s*\)\s*\{\s*lab\[n\]=PDNA_LBL_TO_GAME;\s*act\[n\+\+\]=A_TOGAME;"
    if not re.search(pat, nat):
        return False, "the native branch has no `if (xg_togame_gb_row(is_bank, app_gen3_pc_live(), togame_gb_writable())) { ...TO_GAME... }`"
    if not re.search(r"else\s+" + pat, occ):
        return False, "the occupied (Gen-3 cell) branch has no `else if (xg_togame_gb_row(...)) { ...TO_GAME... }` after the Gen-3 row"
    if nat.find("A_TOGAME") > nat.find("A_RELEASE"):
        return False, "TO GAME must stay before RELEASE in the native branch"
    if "A_TOGAME" in m[emp_i:m.find("PDNA_LBL_CANCEL")]:
        return False, "the empty-cell branch offers TO GAME"
    return True, "ok"


def writable_facts(main: str) -> tuple[bool, str]:
    b = function_body(main, "togame_gb_writable")
    if not b:
        return False, "togame_gb_writable not found"
    if not re.search(r"return\s+pdna_gen12_resident\(\)\s*&&\s*app_can_edit\(\)\s*;", b):
        return False, "togame_gb_writable must be exactly `pdna_gen12_resident() && app_can_edit()` (resident write-capable session only)"
    return True, "ok"


def predicate_facts(gate: str) -> tuple[bool, str]:
    m = re.search(r"bool\s+xg_togame_gb_row\(bool is_bank, bool pc_live, bool gb_writable\)\s*\{\s*return\s+([^;]*);", gate)
    if not m:
        return False, "xg_togame_gb_row not found"
    if re.sub(r"\s+", "", m.group(1)) != "is_bank&&!pc_live&&gb_writable":
        return False, f"xg_togame_gb_row must be `is_bank && !pc_live && gb_writable` (got {m.group(1)!r})"
    return True, "ok"


# ---- G2 --------------------------------------------------------------------------------------
def action_facts(menu: str) -> tuple[bool, str]:
    m = re.search(r"case\s+A_TOGAME:\s*if\s*\(\s*native\s*\|\|\s*" + GB_ROW_CALL +
                  r"\s*\)\s*\{\s*g_togame_req\s*=\s*true\s*;\s*return\s+false\s*;\s*\}\s*return\s+app_inject_to_game\(", strip_comments(menu))
    if not m:
        return False, "case A_TOGAME must be `if (native || xg_togame_gb_row(...)) { g_togame_req = true; return false; }` BEFORE app_inject_to_game(rec)"
    return True, "ok"


# ---- G3 --------------------------------------------------------------------------------------
def consumer_facts(body: str) -> tuple[bool, str]:
    if not body:
        return False, "togame_native_run not found"
    m = re.search(r"if\s*\(\s*xg_togame_gb_row\(\s*true\s*,\s*app_gen3_pc_live\(\)\s*,\s*s_xfer_peer\s*&&\s*s_xfer_peer->accept_down\s*\)\s*\)\s*"
                  r"\(void\)\s*bank_togame_gb\(\s*box\s*,\s*cur\s*,[^;]*;\s*else\s*\(void\)\s*app_bank_togame_native\(", body)
    if not m:
        return False, "togame_native_run must route `if (xg_togame_gb_row(true, app_gen3_pc_live(), s_xfer_peer && s_xfer_peer->accept_down)) bank_togame_gb(...) else app_bank_togame_native(...)`"
    g = re.search(r"if\s*\(\s*!src->is_bank", body)
    if not g or g.start() > m.start():
        return False, "the off-Bank refusal must come before the routing"
    if first(body, "boxoam_suspend") < 0 or first(body, "boxoam_suspend") > m.start() or first(body, "boxoam_resume") < m.start():
        return False, "the routed arms must run inside a boxoam_suspend/resume bracket"
    return True, "ok"


# ---- G4 --------------------------------------------------------------------------------------
ARM_INTERNALS = ("gb_accept_down_hook", "gb_bank_down_bridge", "gb_bridge_restore_up", "bank_down_convert_gb", "gb_bank_down_g3",
                 "gbpc_restore_up", "gbpc_restore_done", "app_inject_to_game", "app_inject_to_game_deferred",
                 "app_bank_defer_delete", "app_bank_clear_slots", "app_commit_pc", "app_commit_all", "sf_write_verified",
                 "box_save", "app_mark_pc_dirty")


def body_facts(body: str) -> tuple[bool, str]:
    if not body:
        return False, "bank_togame_gb not found"
    ok, d = ordered(body, ["app_can_edit", "gb_togame_pick_box", "memcpy", "xg_bank_down_arm", "bank_down_dispatch",
                           "bank_down_consume", "bank_down_g3_run", "boxoam_suspend"])
    if not ok:
        return False, d
    # the SENT TO GAME message is the LAST msg_wait and follows the fresh suspend (the earlier one is GAME FULL)
    if not (body.rfind("msg_wait") > body.rfind("boxoam_suspend") > body.rfind("bank_down_g3_run") and "SENT TO GAME" in body[body.rfind("msg_wait"):]):
        return False, "the SENT TO GAME message must come last, after a fresh boxoam_suspend"
    bad = [c for c in ARM_INTERNALS if first(body, c) >= 0]
    if bad:
        return False, "calls an arm internal / a Gen-3 or deferred write itself (a clone of the drop's arm): " + ", ".join(bad)
    if not re.search(r"landed\s*=\s*\(\s*bd\s*==\s*BANK_DOWN_LANDED\s*\)", body):
        return False, "only BANK_DOWN_LANDED may count as landed for a native cell"
    if not re.search(r"if\s*\(\s*landed\s*&&\s*arm\s*!=\s*XG_DOWN_ARM_EXACT\s*\)\s*\(void\)\s*bank_down_consume\(", body):
        return False, "the bridge consume must be `if (landed && arm != XG_DOWN_ARM_EXACT) bank_down_consume(...)` (EXACT consumes inside itself)"
    if not re.search(r"if\s*\(\s*!landed\s*\)\s*return\s+false\s*;", body):
        return False, "a refused landing must return false before the SENT TO GAME message"
    if not re.search(r"!s_xfer_peer\s*\|\|\s*!s_xfer_peer->accept_down", body):
        return False, "the hook-presence guard (no accept_down -> refuse) is missing"
    return True, "ok"


# ---- G5 --------------------------------------------------------------------------------------
def parity_facts(drop: str, gb: str, g3drop: str) -> tuple[bool, str]:
    dd = y10.drop_dispatch_block(drop)
    if not dd or not gb or not g3drop:
        return False, "a parity body was not found (drop dispatch block / bank_togame_gb / drop_held_down_g3)"
    da, ga = call_args(dd, "bank_down_dispatch"), call_args(gb, "bank_down_dispatch")
    if len(da) != 8 or len(ga) != 8:
        return False, f"bank_down_dispatch must be called with 8 arguments on both sides (drop {len(da)}, menu {len(ga)})"
    if da[6:8] != ["s_orig_box", "s_orig_slot"] or ga[6:8] != ["orig_box", "orig_slot"]:
        return False, f"origin args differ from the carry/cell origin: drop {da[6:8]}, menu {ga[6:8]}"
    if da[3] != "s_held" or ga[3] != "held" or ga[5] != "conv" or da[5] != "conv":
        return False, f"the cell / out buffer args drifted: drop cell {da[3]} out {da[5]}, menu cell {ga[3]} out {ga[5]}"
    darm = re.search(r"xg_bank_down_arm\(\s*bc_kind\(\s*(\w+)\s*\)\s*,\s*([\w>.-]+)\s*,\s*app_gb_session_gen\(\)\s*\)", dd + drop)
    garm = re.search(r"xg_bank_down_arm\(\s*bc_kind\(\s*(\w+)\s*\)\s*,\s*([\w>.-]+)\s*,\s*app_gb_session_gen\(\)\s*\)", gb)
    if not darm or not garm:
        return False, "the arm is not derived by `xg_bank_down_arm(bc_kind(cell), scope, app_gb_session_gen())` on both sides"
    if (darm.group(1), garm.group(1)) != ("s_held", "held") or garm.group(2) != "gbs.scope":
        return False, f"the arm derivation drifted: drop {darm.groups()}, menu {garm.groups()}"
    # the drop's own EXACT-vs-tail split is the one the menu mirrors
    if "arm == XG_DOWN_ARM_EXACT" not in drop or "arm != XG_DOWN_ARM_EXACT" not in gb:
        return False, "the EXACT-consumes-inside / bridge-consumes-in-the-tail split is not mirrored"
    if not re.search(r"\(void\)\s*bank_down_consume\(\s*s_orig_box\s*,\s*s_orig_slot\s*,\s*s_held\s*\)", drop):
        return False, "drop_held's LANDED tail no longer calls bank_down_consume(s_orig_box, s_orig_slot, s_held)"
    if not re.search(r"bank_down_consume\(\s*orig_box\s*,\s*orig_slot\s*,\s*held\s*\)", gb):
        return False, "bank_togame_gb no longer calls bank_down_consume(orig_box, orig_slot, held)"
    # the Gen-3 cell: ONE function, both callers, the same shape
    dg = call_args(g3drop, "bank_down_g3_run")
    mg = call_args(gb, "bank_down_g3_run")
    if dg != ["box", "s_held", "s_orig_box", "s_orig_slot"]:
        return False, f"drop_held_down_g3 must call bank_down_g3_run(box, s_held, s_orig_box, s_orig_slot) (got {dg})"
    if mg != ["dbox", "held", "orig_box", "orig_slot"]:
        return False, f"bank_togame_gb must call bank_down_g3_run(dbox, held, orig_box, orig_slot) (got {mg})"
    return True, "ok"


# ---- G6 --------------------------------------------------------------------------------------
def pick_facts(body: str) -> tuple[bool, str]:
    if not body:
        return False, "gb_togame_pick_box not found"
    checks_ = [
        (r"if\s*\(\s*b\s*==\s*party\s*\)\s*continue\s*;", "never picks the party pseudo-box"),
        (r"gbs_box_writable\(\s*&g_ed->s\s*,\s*b\s*\)\s*!=\s*GBS_OK\s*\)\s*continue", "skips boxes gbs_box_writable refuses"),
        (r"gbs_load_list\([^;]*!=\s*GBS_OK\s*\)\s*continue", "skips a box whose list will not stage"),
        (r"count\s*>=\s*0\s*&&\s*count\s*<\s*gb_list_capacity\(", "requires a trusted count strictly below capacity"),
        (r"\*out_slot\s*=\s*count\s*;", "reports the append slot (== count)"),
    ]
    for pat, what in checks_:
        if not re.search(pat, body):
            return False, f"gb_togame_pick_box: missing/changed -- {what}"
    return True, "ok"


def real_texts():
    main, box, gen12, gate = MAIN.read_text(), BOX.read_text(), GEN12.read_text(), GATE.read_text()
    return dict(main=main, menu=function_body(main, "app_mon_menu"), gate=gate,
                helper=function_body(box, "togame_native_run"), gb=function_body(box, "bank_togame_gb"),
                drop=function_body(box, "drop_held"), g3drop=function_body(box, "drop_held_down_g3"),
                pick=function_body(gen12, "gb_togame_pick_box"))


def run_real() -> None:
    t = real_texts()
    for k in ("menu", "helper", "gb", "drop", "g3drop", "pick"):
        check(bool(t[k]), f"{k}: body not found")
    for label, fn, arg in (("G1 rows", rows_facts, t["menu"]),
                           ("G1 writable", writable_facts, t["main"]),
                           ("G1 predicate", predicate_facts, t["gate"]),
                           ("G2 action", action_facts, t["menu"]),
                           ("G3 consumer", consumer_facts, t["helper"]),
                           ("G4 body", body_facts, t["gb"]),
                           ("G6 pick", pick_facts, t["pick"])):
        ok, d = fn(arg)
        check(ok, f"{label}: {d}")
    ok, d = parity_facts(t["drop"], t["gb"], t["g3drop"])
    check(ok, f"G5 parity: {d}")


def self_test() -> None:
    t = real_texts()
    menu, main, gate, hp, gb, dr, g3, pk = (t["menu"], t["main"], t["gate"], t["helper"], t["gb"], t["drop"], t["g3drop"], t["pick"])
    row = "if (xg_togame_gb_row(is_bank, app_gen3_pc_live(), togame_gb_writable())) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }"
    muts = [
        ("MUT G1a: the native GB row removed", rows_facts, mutate(menu, row, "", )),
        ("MUT G1b: the native GB row loses its gate", rows_facts, mutate(menu, row, "{ lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }")),
        ("MUT G1c: the Gen-3 cell's GB row loses its gate", rows_facts,
         mutate(menu, "else if (xg_togame_gb_row(is_bank, app_gen3_pc_live(), togame_gb_writable())) { lab[n]=PDNA_LBL_TO_GAME;",
                "else { lab[n]=PDNA_LBL_TO_GAME;")),
        ("MUT G1d: the writable gate forgets app_can_edit (an Everdrive would see the row)", writable_facts,
         mutate(main, "return pdna_gen12_resident() && app_can_edit();", "return pdna_gen12_resident();")),
        ("MUT G1e: the writable gate forgets residency (the read-only mount would see the row)", writable_facts,
         mutate(main, "return pdna_gen12_resident() && app_can_edit();", "return app_can_edit();")),
        ("MUT G1f: the predicate drops !pc_live (both rows on one cell)", predicate_facts,
         mutate(gate, "return is_bank && !pc_live && gb_writable;", "return is_bank && gb_writable;")),
        ("MUT G1g: TO GAME leaks onto an EMPTY cell", rows_facts,
         mutate(menu, "if (n == 0) { snd_deny(); return false; }", "lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; if (n == 0) { snd_deny(); return false; }")),
        ("MUT G2a: a Gen-3 cell in a GB session injects into the Gen-3 save", action_facts,
         mutate(menu, "case A_TOGAME:  if (native || xg_togame_gb_row(", "case A_TOGAME:  if (native || 0 && xg_togame_gb_row(")),
        ("MUT G2b: the request is never set", action_facts, mutate(menu, "g_togame_req = true; return false; }   ", "return false; }   ")),
        ("MUT G3a: the consumer never routes to bank_togame_gb", consumer_facts, mutate(hp, "(void)bank_togame_gb(", "(void)app_bank_togame_native(")),
        ("MUT G3b: the routing predicate loses the accept_down presence", consumer_facts,
         mutate(hp, "s_xfer_peer && s_xfer_peer->accept_down", "1")),
        ("MUT G3c: no OAM bracket", consumer_facts, hp.replace("boxoam_suspend();", "", 1)),
        ("MUT G4a: the write gate removed", body_facts, mutate(gb, "if (!app_can_edit())", "if (0)")),
        ("MUT G4b: the destination pick removed", body_facts, mutate(gb, "gb_togame_pick_box(&slot)", "0")),
        ("MUT G4c: the bridge consume dropped", body_facts,
         mutate(gb, "if (landed && arm != XG_DOWN_ARM_EXACT) (void)bank_down_consume(orig_box, orig_slot, held);", "")),
        ("MUT G4d: the EXACT arm consumed twice (consume for every arm)", body_facts,
         mutate(gb, "if (landed && arm != XG_DOWN_ARM_EXACT)", "if (landed)")),
        ("MUT G4e: a refused landing still reports SENT TO GAME", body_facts, mutate(gb, "if (!landed) return false;", "")),
        ("MUT G4f: the menu defers the Bank delete (Gen-3 style)", body_facts,
         mutate(gb, "(void)bank_down_consume(orig_box, orig_slot, held);", "app_bank_defer_delete(orig_box, orig_slot, held);")),
        ("MUT G4g: the menu writes the native cell itself (arm clone)", body_facts,
         mutate(gb, "const BankDownResult bd = bank_down_dispatch(", "gb_accept_down_hook(dbox, held); const BankDownResult bd = bank_down_dispatch(")),
        ("MUT G4h: SENT TO GAME without the fresh OAM suspend", body_facts,
         mutate(gb, "boxoam_suspend();", "")),
        ("MUT G5a: the menu passes a different origin to the dispatch", parity_facts,
         (dr, mutate(gb, "held, held, conv, orig_box, orig_slot)", "held, held, conv, dbox, slot)"), g3)),
        ("MUT G5b: the drop stops passing its carry origin", parity_facts,
         (mutate(dr, "conv, s_orig_box, s_orig_slot)", "conv, -1, -1)"), gb, g3)),
        ("MUT G5c: the menu derives the arm from a different scope", parity_facts,
         (dr, mutate(gb, "xg_bank_down_arm(bc_kind(held), gbs.scope,", "xg_bank_down_arm(bc_kind(held), BOXSCOPE_PC,"), g3)),
        ("MUT G5d: the Gen-3 cell path is cloned (drop stops using bank_down_g3_run)", parity_facts,
         (dr, gb, mutate(g3, "bank_down_g3_run(box, s_held, s_orig_box, s_orig_slot)", "gb_bank_down_g3(box, s_held)"))),
        ("MUT G5e: the menu's Gen-3 path uses another origin", parity_facts,
         (dr, mutate(gb, "bank_down_g3_run(dbox, held, orig_box, orig_slot)", "bank_down_g3_run(dbox, held, dbox, slot)"), g3)),
        ("MUT G5f: the drop's tail stops consuming through the shared helper", parity_facts,
         (mutate(dr, "(void)bank_down_consume(s_orig_box, s_orig_slot, s_held);", ";"), gb, g3)),
        ("MUT G6a: the pick may choose the party", pick_facts, mutate(pk, "if (b == party) continue;", "")),
        ("MUT G6b: the pick ignores a full box", pick_facts, mutate(pk, "count < gb_list_capacity(", "count < 99 + gb_list_capacity(")),
        ("MUT G6c: the pick trusts an unreadable count", pick_facts, mutate(pk, "count >= 0 && count <", "count <")),
        ("MUT G6d: the pick reports the wrong slot", pick_facts, mutate(pk, "*out_slot = count;", "*out_slot = 0;")),
    ]
    for label, fn, arg in muts:
        ok, d = fn(*arg) if isinstance(arg, tuple) else fn(arg)
        check(not ok, f"{label} should have been caught but was not")
        print(f"  {label}: {'CAUGHT' if not ok else 'MISSED'} ({d})")


def main() -> int:
    run_real()
    self_test()
    print(f"host_y12_togame_gb_sites_test: {checks} checks, {len(fails)} failed")
    for f in fails:
        print("  FAIL:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
