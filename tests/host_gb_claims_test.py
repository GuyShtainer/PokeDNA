#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_claims_test.py -- self-test for tools/gb_claims.py (BACKLOG #184).

Pure Python + numpy/PIL, no emulator/mgba import (same house rule the other harness
self-tests follow: "harness scripts never `import mgba` at module level"). Covers:

  (a) render("PASTE HERE", proportional=True/False) into a synthetic frame at a
      RANDOM position with a RANDOM ink colour on a noisy (multi-colour, small-palette)
      background, and find() locates it -- both font modes.
  (b) a near-miss ("PASTE HERO" instead of "PASTE HERE") painted the SAME way is NOT
      found -- proves this is an exact bitmap match, not a fuzzy/substring one.
  (c) find(proportional=None) (the caller-agnostic mode Session.shot() actually uses)
      finds text drawn in EITHER font without being told which.
  (d) claim_absent semantics via check(): a claim that IS present fails claim_absent;
      a claim that is genuinely absent passes it.
  (e) a real PNG fixture, if one is tracked under docs/shots -- else a second
      synthetic frame stands in (docs/ is gitignored in this repo, so a tracked
      fixture is not guaranteed to exist in every checkout).

Run directly:

    python3 tests/host_gb_claims_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #184).
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402
import gb_claims  # noqa: E402


def make_noisy_frame(rng, h=160, w=240, palette_size=24):
    """A small-palette noisy background -- realistic for a GBA capture (<=256
    colours, gb_claims.find()'s own documented assumption), not per-pixel 24-bit
    noise (which would blow past that cap and is not what mGBA ever produces)."""
    palette = rng.integers(0, 256, size=(palette_size, 3), dtype=np.uint8)
    idx = rng.integers(0, palette_size, size=(h, w))
    return palette[idx]


def paint_text(frame, text, x0, y0, colour, proportional, bg=(10, 10, 10)):
    mask = gb_claims.render(text, proportional=proportional)
    h, w = mask.shape
    frame[y0:y0 + h, x0:x0 + w] = bg
    frame[y0:y0 + h, x0:x0 + w][mask] = np.array(colour, dtype=np.uint8)
    return mask.shape


def check_random_position(rng, text, proportional, label, failures):
    frame = make_noisy_frame(rng)
    colour = tuple(int(c) for c in rng.integers(40, 256, size=3))
    # Keep well clear of the frame edge so the whole glyph run fits.
    h, w = gb_claims.render(text, proportional=proportional).shape
    x0 = int(rng.integers(0, 240 - w))
    y0 = int(rng.integers(0, 160 - h))
    paint_text(frame, text, x0, y0, colour, proportional)

    hits = gb_claims.find(frame, text, proportional=None)  # caller-agnostic mode
    if not any(x == x0 and y == y0 for x, y, c in hits):
        failures.append(f"{label}: render({text!r}, proportional={proportional}) at "
                         f"({x0},{y0}) colour {colour} was NOT found by "
                         f"find(proportional=None); hits={hits}")
        return
    match = [c for x, y, c in hits if x == x0 and y == y0][0]
    if match != colour:
        failures.append(f"{label}: found at the right position but wrong colour "
                         f"{match} != {colour}")
        return

    # near-miss: same position/colour, one character changed -- must not match.
    near = text[:-1] + ("O" if text[-1] != "O" else "X")
    near_hits = gb_claims.find(frame, near, proportional=None)
    if any(x == x0 and y == y0 for x, y, c in near_hits):
        failures.append(f"{label}: near-miss {near!r} incorrectly matched at "
                         f"({x0},{y0}) -- find() is not exact")
        return

    print(f"  ok: {label} render({text!r}, proportional={proportional}) found at "
          f"({x0},{y0}) colour {colour}; near-miss {near!r} correctly rejected")


def check_claim_absent(rng, failures):
    frame = make_noisy_frame(rng)
    present_text = "KEEP"
    absent_text = "DISCARD ALL"
    colour = (255, 255, 255)
    h, w = gb_claims.render(present_text, proportional=True).shape
    x0, y0 = 50, 30
    paint_text(frame, present_text, x0, y0, colour, proportional=True)

    fails = gb_claims.check(frame, claim=present_text)
    if fails:
        failures.append(f"claim_absent setup: claim={present_text!r} unexpectedly "
                         f"failed: {fails}")
        return
    fails = gb_claims.check(frame, claim_absent=present_text)
    if not fails:
        failures.append(f"claim_absent: {present_text!r} IS on the frame but "
                         f"claim_absent did not fail")
        return
    fails = gb_claims.check(frame, claim_absent=absent_text)
    if fails:
        failures.append(f"claim_absent: {absent_text!r} is genuinely absent but "
                         f"claim_absent reported a failure: {fails}")
        return
    print("  ok: check() claim_absent semantics (present text fails claim_absent, "
          "genuinely-absent text passes it)")


def check_real_fixture(failures):
    """A real PNG from docs/shots, if this checkout happens to have one tracked
    (docs/ is gitignored -- see docs/kb/gitignore-internal-dev-docs.md's convention
    -- so most checkouts will not). Falls back to a second synthetic frame so the
    test still exercises find() against a full PIL round-trip (path -> Image ->
    ndarray), not just an in-memory ndarray, in every checkout."""
    shots_dir = ROOT / "docs" / "shots" / "gb"
    manifest_path = shots_dir / "manifest.json"
    fixture = None
    if manifest_path.is_file():
        import json
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            for entry in manifest.get("shots", []):
                p = shots_dir / entry.get("file", "")
                if p.is_file():
                    fixture = p
                    break
        except Exception:
            fixture = None

    if fixture is not None:
        # We don't know this fixture's own text in advance (it's whatever the
        # checkout happens to have) -- the only thing we can assert without
        # coupling to a specific shot is that find() runs to completion and
        # returns a list (no exception) for an arbitrary short claim.
        hits = gb_claims.find(fixture, "A", proportional=None)
        if not isinstance(hits, list):
            failures.append(f"real fixture {fixture}: find() did not return a list")
            return
        print(f"  ok: real fixture {fixture.relative_to(ROOT)} -- find() ran clean "
              f"({len(hits)} hit(s) for a single-glyph probe)")
    else:
        rng = np.random.default_rng(999)
        frame = make_noisy_frame(rng)
        paint_text(frame, "REAL FIXTURE STAND-IN", 10, 100, (0, 255, 0), True)
        from PIL import Image
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "stand_in.png"
            Image.fromarray(frame, "RGB").save(path)
            hits = gb_claims.find(path, "REAL FIXTURE STAND-IN", proportional=None)
            if not any(x == 10 and y == 100 for x, y, c in hits):
                failures.append("stand-in fixture (no tracked docs/shots PNG in this "
                                 f"checkout): find() via a real file path failed; hits={hits}")
                return
        print("  ok: no tracked docs/shots/gb PNG in this checkout (docs/ is "
              "gitignored) -- stand-in file-path round trip passed instead")


def main() -> int:
    failures: list[str] = []
    rng = np.random.default_rng(20260922)

    check_random_position(rng, "PASTE HERE", True, "proportional", failures)
    check_random_position(rng, "PASTE HERE", False, "fixed (tonc sys8)", failures)
    check_claim_absent(rng, failures)
    check_real_fixture(failures)

    if failures:
        print(f"\nhost_gb_claims_test: {len(failures)} FAILURE(S):", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("\nhost_gb_claims_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
