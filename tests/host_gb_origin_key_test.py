#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_origin_key_test.py -- BACKLOG #172: the stored Gold/Silver-or-Red/Blue/
Yellow origin answer under /PokeDNA/xfer/<hex>.og used to be keyed ONLY by the save's
on-card PATH (gb_origin_key(), source/pdna_gen12.c), so a DIFFERENT save copied onto
the same path silently inherited the old answer instead of re-asking. The fix adds a
4-byte fingerprint of the save's own identity (player name + public trainer id, read
off the live mount's already-parsed Gb12Mount.player/.tid -- no new save parser)
stored alongside the answer byte in the .og file; a mismatch or an old 1-byte file
re-asks and rewrites. It also caches the answered origin in Gb12Edit
(origin_write_failed/origin_cached) for the rest of the mount when persisting the
answer fails, so a write failure never causes a SECOND prompt (which could pick a
different answer and mis-route S150-7's exact DOWN mid-mount).

gb_origin_for_save() lives inside pdna_gen12.c's `#ifndef PDNA_GEN12_HOST` region (it
calls FatFs + the tonc UI picker) and is therefore not part of the pure-C host build
tests/host_gen12_test.c compiles -- same reason tests/host_gb_write_gate_test.py
audits this exact file with pure-text/structural checks instead of executing it. This
test follows the same idiom: (A) structural checks over the REAL shipped source that
the described logic is actually present (not a second, driftable copy of it), and (B)
an independent behavioral re-derivation of the FNV-1a-64 fingerprint + the 5-byte file
format's decision table, run against the three scenarios BACKLOG #172 names:
  1. the SAME save re-opened at the same path -> matching fingerprint -> no re-prompt.
  2. a DIFFERENT save landing on the same path -> mismatching fingerprint -> re-ask.
  3. an OLD-FORMAT (1-byte, no fingerprint) file -> too short -> re-ask + rewrite.

Run directly:

    python3 tests/host_gb_origin_key_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (b172).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "source" / "pdna_gen12.c"

FAILS: list[str] = []


def check(cond: bool, msg: str) -> None:
    if not cond:
        FAILS.append(msg)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def extract_function_body(text: str, func_name: str) -> str:
    m = re.search(r"\b" + re.escape(func_name) + r"\s*\([^;{]*\)\s*\{", text)
    if not m:
        return ""
    start = m.start()
    i = m.end() - 1
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


# ---- FNV-1a-64, independently re-derived (the SAME constants gb_sidecar.c's
# gbsc_key() and pdna_gen12.c's gb_fnv64() both use) -------------------------------

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv64(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def fingerprint(tid: int, player: str) -> int:
    """Mirrors gb_origin_fingerprint(): 2 LE bytes of tid, then the player name's
    raw bytes up to (but not including) its NUL, then the low 32 bits of FNV-1a-64
    over that buffer."""
    buf = bytes([tid & 0xFF, (tid >> 8) & 0xFF]) + player.encode("utf-8")
    return fnv64(buf) & 0xFFFFFFFF


def encode_og(origin: int, fp: int) -> bytes:
    """Mirrors the NEW 5-byte .og format: answer byte + LE32 fingerprint."""
    return bytes([origin, fp & 0xFF, (fp >> 8) & 0xFF, (fp >> 16) & 0xFF, (fp >> 24) & 0xFF])


BC_ORIGIN_UNKNOWN, BC_ORIGIN_RED, BC_ORIGIN_BLUE, BC_ORIGIN_YELLOW = 0, 1, 2, 3
BC_ORIGIN_GOLD, BC_ORIGIN_SILVER, BC_ORIGIN_CRYSTAL = 4, 5, 6
GB_GEN1, GB_GEN2 = 1, 2  # only used to pick the right valid-value set below


def gen_ok(gen: int, byte0: int) -> bool:
    if gen == GB_GEN1:
        return byte0 in (BC_ORIGIN_RED, BC_ORIGIN_BLUE, BC_ORIGIN_YELLOW)
    return byte0 in (BC_ORIGIN_GOLD, BC_ORIGIN_SILVER, BC_ORIGIN_CRYSTAL)


def decide(gen: int, current_fp: int, filebuf: bytes | None) -> str:
    """Re-derivation of gb_origin_for_save()'s read-side branch (crystal short-
    circuit and the write side are out of scope here -- this is exactly the part
    BACKLOG #172 changed). Returns "ANSWERED" (no re-prompt, buf[0] stands) or
    "REASK" (re-prompt and rewrite)."""
    if filebuf is not None and len(filebuf) >= 5 and gen_ok(gen, filebuf[0]):
        stored_fp = filebuf[1] | (filebuf[2] << 8) | (filebuf[3] << 16) | (filebuf[4] << 24)
        if stored_fp == current_fp:
            return "ANSWERED"
    return "REASK"


def main() -> int:
    if not SRC.exists():
        print(f"SKIP: {SRC} not found")
        return 0
    raw = SRC.read_text(errors="replace")
    text = strip_comments(raw)

    # ---- A. structural checks over the real shipped source ------------------------

    fp_body = extract_function_body(text, "gb_origin_fingerprint")
    check(bool(fp_body), "gb_origin_fingerprint() not found in source/pdna_gen12.c")
    if fp_body:
        check("g_m" in fp_body and "tid" in fp_body,
              "gb_origin_fingerprint must read the live mount's tid (Gb12Mount.tid), not a new parse")
        check("player" in fp_body,
              "gb_origin_fingerprint must read the live mount's player name (Gb12Mount.player)")
        check("gb_fnv64(" in fp_body,
              "gb_origin_fingerprint must reuse gb_fnv64 (the existing FNV helper), not a second hash")

    key_body = extract_function_body(text, "gb_origin_key")
    check(bool(key_body), "gb_origin_key() not found")
    if key_body:
        check("gb_fnv64(" in key_body,
              "gb_origin_key must reuse gb_fnv64 too -- one hash implementation, not two")

    body = extract_function_body(text, "gb_origin_for_save")
    check(bool(body), "gb_origin_for_save() not found in source/pdna_gen12.c")
    if body:
        check("origin_write_failed" in body,
              "gb_origin_for_save must consult/set origin_write_failed (BACKLOG #172 mount-wide cache)")
        # The cache check must come BEFORE the .og file is ever opened, so a prior
        # write failure this mount skips file I/O entirely (no re-prompt, no
        # divergent re-answer).
        cache_pos = body.find("origin_write_failed")
        xr_pos = body.find("xr_path_for_name")
        check(cache_pos != -1 and xr_pos != -1 and cache_pos < xr_pos,
              "the origin_write_failed cache check must run BEFORE xr_path_for_name/file I/O")
        check("len >= 5" in body,
              "the .og read gate must require the 5-byte format (answer + 4-byte fingerprint), not len >= 1")
        check("len >= 1" not in body,
              "the OLD 1-byte-only gate must be gone -- an old-format file is exactly what must now re-ask")
        check("stored_fp == fp" in body or "fp == stored_fp" in body,
              "gb_origin_for_save must compare the stored fingerprint against the current save's own")
        # Both persistence-failure branches (mkdir refusal, verified-write refusal)
        # must cache the answer for the rest of the mount.
        set_count = len(re.findall(r"origin_write_failed\s*=\s*true", body))
        check(set_count >= 2,
              f"expected the mkdir-fail AND write-fail branches to both set origin_write_failed=true "
              f"(found {set_count} site(s))")
        cached_count = len(re.findall(r"origin_cached\s*=\s*\(uint8_t\)picked", body))
        check(cached_count >= 2,
              f"expected both failure branches to also cache the answered byte (found {cached_count} site(s))")

    end_struct = text.find("} Gb12Edit;")
    start_struct = max(0, end_struct - 800)
    edit_struct = text[start_struct:end_struct + len("} Gb12Edit;")] if end_struct != -1 else ""
    check(bool(edit_struct), "Gb12Edit struct definition not found")
    check("origin_write_failed" in edit_struct and "origin_cached" in edit_struct,
          "Gb12Edit must carry origin_write_failed/origin_cached fields")

    check("ed->origin_write_failed = false" in text and "ed->origin_cached = 0" in text,
          "a freshly latched g_ed must explicitly zero the #172 cache (never inherit stale arena garbage, "
          "same discipline as romgs_ready/learn_ready)")

    # ---- B. behavioral re-derivation: the three named scenarios --------------------

    # Scenario 1: the SAME save re-opened at the same path -> no re-prompt.
    fp_a = fingerprint(0x1234, "GUY")
    stored = encode_og(BC_ORIGIN_GOLD, fp_a)
    check(decide(GB_GEN2, fp_a, stored) == "ANSWERED",
          "same save re-opened (identical tid+player) must NOT re-prompt")

    # Scenario 2: a DIFFERENT save copied onto the same path -> fingerprint
    # mismatch -> re-ask. (BACKLOG #172's whole reason to exist.)
    fp_b = fingerprint(0x5678, "RIVAL")
    check(fp_a != fp_b, "test fixture bug: the two scenario-2 saves must not collide")
    check(decide(GB_GEN2, fp_b, stored) == "REASK",
          "a different save at the same path must re-ask (fingerprint mismatch)")

    # Scenario 2b: same tid, different name (and vice versa) must also mismatch --
    # the fingerprint has to depend on BOTH fields, not just one.
    fp_same_tid_diff_name = fingerprint(0x1234, "RIVAL")
    fp_diff_tid_same_name = fingerprint(0x5678, "GUY")
    check(fp_same_tid_diff_name != fp_a, "fingerprint must depend on the player name, not tid alone")
    check(fp_diff_tid_same_name != fp_a, "fingerprint must depend on the trainer id, not the name alone")

    # Scenario 3: an OLD-FORMAT (1-byte) file -> too short -> re-ask + rewrite.
    old_format = bytes([BC_ORIGIN_GOLD])
    check(decide(GB_GEN2, fp_a, old_format) == "REASK",
          "an old 1-byte .og file (no fingerprint) must re-ask, not silently trust the bare byte")

    # Scenario 4: no file at all (first-ever lift) -> re-ask (sanity: decide() must
    # not crash or silently answer on None).
    check(decide(GB_GEN2, fp_a, None) == "REASK", "no existing .og file must re-ask")

    # Scenario 5: a value that doesn't belong to this generation (corrupt / cross-
    # gen leftover) must also re-ask even with a matching fingerprint.
    wrong_gen_file = encode_og(BC_ORIGIN_RED, fp_a)  # Gen-1 answer under a Gen-2 read
    check(decide(GB_GEN2, fp_a, wrong_gen_file) == "REASK",
          "a stored answer that doesn't belong to this generation must re-ask even if the fingerprint matches")

    if FAILS:
        print(f"host_gb_origin_key_test: {len(FAILS)} FAIL(s)")
        for f in FAILS:
            print(f"  !! {f}")
        return 1
    print("host_gb_origin_key_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
