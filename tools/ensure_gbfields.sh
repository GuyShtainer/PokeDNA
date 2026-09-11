#!/usr/bin/env bash
# BACKLOG #109 — regenerate source/gb_fields.c + gb_flags.c when stale, BEFORE the
# Makefile's CFILES $(wildcard) is evaluated (a target-based prerequisite, like
# check-data-tables, is too late for a file that doesn't exist yet at parse time).
# Called from the Makefile via a parse-time $(shell ...); prints STATUS TO STDERR ONLY
# (its stdout must stay empty so the $(shell ...) call site can swallow it cleanly) and
# always exits 0 EXCEPT when regeneration was actually attempted and failed -- a stale
# table must never be kept silently (it bit the gbdata merge: 22/170 fresh objects,
# three core tests failed until tools/gen_gbfields.py was re-run by hand).
#
# tools/gen_gbfields.py itself never opens docs/GEN12-PARITY-DESIGN.md at runtime -- its
# check_off/check_len cross-checks are numeric literals baked into the script (the doc is
# only cited in comments/error text as WHERE those literals came from). So the doc's
# presence changes nothing about correctness; it is still treated as an input here (a
# clean-room record: if the doc changes, the pinned literals in the generator may need a
# human look), just not one the generator reads.
#
# The .sym files ARE load-bearing (load_sym() opens them). They live under the
# git-ignored assets/upstream/*/symbols/ (reference-only decomp checkouts, never
# committed -- see README's "Graphics assets" section), so a genuinely fresh clone has
# NONE of them. That is the normal, IP-clean state of the tree, not a defect: this script
# leaves gb_fields.c/gb_flags.c ungenerated and prints a loud NOTE; the build still links
# via source/gb_fields_fallback.c / gb_flags_fallback.c's committed __attribute__((weak))
# fallback (every Gen-1/2 field reports "the game lacks it").
set -u
cd "$(dirname "$0")/.." || exit 1

OUT1=source/gb_fields.c
OUT2=source/gb_flags.c
GEN=tools/gen_gbfields.py
DESIGN_DOC=docs/GEN12-PARITY-DESIGN.md

SYMS="assets/upstream/pokered/symbols/pokered.sym
assets/upstream/pokeyellow/symbols/pokeyellow.sym
assets/upstream/pokegold/symbols/pokegold.sym
assets/upstream/pokecrystal/symbols/pokecrystal.sym"

mtime() {
  # BSD stat (macOS) first, then GNU stat (Linux / the Docker build).
  stat -L -f %m "$1" 2>/dev/null || stat -L -c %Y "$1" 2>/dev/null
}

missing_sym=0
missing_sym_path=""
for s in $SYMS; do
  if [ ! -f "$s" ]; then
    missing_sym=1
    missing_sym_path="$s"
    break
  fi
done

if [ "$missing_sym" = 1 ]; then
  {
    echo "ensure_gbfields: MISSING $missing_sym_path"
  } 1>&2
  exit 1
fi

newest=0
for f in "$GEN" $SYMS; do
  m=$(mtime "$f")
  [ -n "${m:-}" ] && [ "$m" -gt "$newest" ] && newest=$m
done
doc_present=0
if [ -f "$DESIGN_DOC" ]; then
  doc_present=1
  m=$(mtime "$DESIGN_DOC")
  [ -n "${m:-}" ] && [ "$m" -gt "$newest" ] && newest=$m
fi

need=0
for o in "$OUT1" "$OUT2"; do
  if [ ! -f "$o" ]; then
    need=1
  else
    m=$(mtime "$o")
    if [ -z "${m:-}" ] || [ "$m" -lt "$newest" ]; then need=1; fi
  fi
done

[ "$need" = 1 ] || exit 0

if [ "$doc_present" = 0 ]; then
  {
    echo "*** NOTE: docs/GEN12-PARITY-DESIGN.md absent (a fresh clone -- docs/ is git-ignored,"
    echo "***       local-only engineering notes). gen_gbfields.py's cross-checks are pinned"
    echo "***       numeric literals baked into the script, not a runtime read of that doc, so"
    echo "***       this changes nothing about correctness -- generating from the .sym files alone."
  } 1>&2
fi

if python3 "$GEN" 1>&2; then
  echo "  gbfields: regenerated $OUT1 + $OUT2 from 4 .sym files" 1>&2
  exit 0
else
  rc=$?
  {
    echo "*** FATAL: $GEN failed (exit $rc) -- see the error above."
    echo "***        $OUT1 / $OUT2 were NOT (re)written; any pre-existing copies are left"
    echo "***        exactly as they were, which may be stale. Fix the generator or its"
    echo "***        .sym-file inputs before building."
  } 1>&2
  exit 1
fi
