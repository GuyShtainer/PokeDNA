#!/usr/bin/env bash
# check_no_desc_text.sh -- post-link proof that an ARTLESS PokeDNA build ships zero
# Game Freak description text (BACKLOG #19's last item, docs/AUDIT-2026-09-05-
# backlog-3-19.md section B; desc_gate.h / data_desc_shim.c is the compile-time gate
# this script verifies actually held). Same spirit as the EWRAM-usage guard: a build
# that silently regresses on this is worse than one that fails loudly.
#
# GUARD INTEGRITY (review finding, 2026-09-05): a script that inspects NOTHING must
# never report OK. Earlier versions treated a missing .gba/.elf as "skip" (exit 0)
# and let a missing arm-none-eabi-nm make the symbol check silently vacuous
# (`cmd 2>/dev/null | grep -c` -> 0 hits when `cmd` itself doesn't exist). Both are
# now hard failures with their own message -- "the check didn't run" is never the
# same as "the check ran and passed".
#
# Usage: tools/check_no_desc_text.sh <PROJ-basename>
#   e.g. tools/check_no_desc_text.sh PokeDNA-artless
# Checks <basename>.gba (three verbatim Game Freak sentences, via `strings`) and
# <basename>.elf (the three description arrays' own symbols, via `arm-none-eabi-nm`)
# in the current directory. Exits non-zero -- failing the calling `make` recipe -- the
# moment either check finds a hit, is missing its input, or is missing its own tool.
set -euo pipefail

BASE="${1:?usage: check_no_desc_text.sh <PROJ-basename>}"
GBA="$BASE.gba"
ELF="$BASE.elf"

# Three fragments, one from each of the three gated tables (item/ability/move), so a
# partial gate (e.g. only s_itemdesc caught) still fails loudly.
FRAGMENTS=(
  "restores HP by 60 points"
  "Summons rain in battle"
  "Pounds the foe with forelegs"
)

fail=0

if [ -f "$GBA" ]; then
  for frag in "${FRAGMENTS[@]}"; do
    n=$(strings "$GBA" | grep -c -F "$frag" || true)
    if [ "$n" -gt 0 ]; then
      echo "*** FATAL: $GBA still contains verbatim Game Freak description text:"
      echo "***          \"$frag\" ($n hit(s))"
      fail=1
    fi
  done
else
  echo "*** FATAL: $GBA not found -- the strings check did NOT run, so this is not a pass" >&2
  fail=1
fi

if ! command -v arm-none-eabi-nm >/dev/null 2>&1; then
  echo "*** FATAL: arm-none-eabi-nm not on PATH -- the symbol check did NOT run, so this is not a pass" >&2
  fail=1
elif [ -f "$ELF" ]; then
  n=$(arm-none-eabi-nm "$ELF" | grep -c -E 's_itemdesc|s_mvdesc|s_abilitydesc' || true)
  if [ "$n" -gt 0 ]; then
    echo "*** FATAL: $ELF still links s_itemdesc/s_mvdesc/s_abilitydesc ($n symbol(s))"
    fail=1
  fi
  # Art-symbol denylist: generated art arrays that must never reach an artless link.
  # daycare_bg is the porymap/decomp-tileset render (release audit 2026-10-08).
  n=$(arm-none-eabi-nm "$ELF" | grep -c -w -E 'daycare_bg' || true)
  if [ "$n" -gt 0 ]; then
    echo "*** FATAL: $ELF links a generated art array ($n symbol(s) from the denylist)"
    fail=1
  fi
else
  echo "*** FATAL: $ELF not found -- the symbol check did NOT run, so this is not a pass" >&2
  fail=1
fi

# Size ceiling: the artless ROM is ~919 KB; a 50 KB+ jump is an art array leaking in
# (the daycare render added 52 KB without tripping the text checks). Raise deliberately.
MAX_GBA_BYTES=940000
if [ -f "$GBA" ]; then
  sz=$(stat -f%z "$GBA" 2>/dev/null || stat -c%s "$GBA")
  if [ "$sz" -gt "$MAX_GBA_BYTES" ]; then
    echo "*** FATAL: $GBA is $sz bytes, over the artless ceiling of $MAX_GBA_BYTES -- an art array leaked in"
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "*** An artless build must not carry the embedded description tables -- see"
  echo "***   source/desc_gate.h, source/data_desc_shim.c, and"
  echo "***   docs/AUDIT-2026-09-05-backlog-3-19.md section B."
  exit 1
fi

echo "check_no_desc_text: OK -- no Game Freak description text in $BASE.gba/.elf"
