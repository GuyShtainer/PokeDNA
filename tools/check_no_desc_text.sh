#!/usr/bin/env bash
# check_no_desc_text.sh -- post-link proof that an ARTLESS PokeDNA build ships zero
# Game Freak description text (BACKLOG #19's last item, docs/AUDIT-2026-09-05-
# backlog-3-19.md section B; desc_gate.h / data_desc_shim.c is the compile-time gate
# this script verifies actually held). Same spirit as the EWRAM-usage guard: a build
# that silently regresses on this is worse than one that fails loudly.
#
# Usage: tools/check_no_desc_text.sh <PROJ-basename>
#   e.g. tools/check_no_desc_text.sh PokeDNA-artless
# Checks <basename>.gba (three verbatim Game Freak sentences, via `strings`) and
# <basename>.elf (the three description arrays' own symbols, via `arm-none-eabi-nm`)
# in the current directory. Exits non-zero -- failing the calling `make` recipe -- the
# moment either check finds a hit.
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
  echo "*** check_no_desc_text.sh: $GBA not found -- skipping the strings check" >&2
fi

if [ -f "$ELF" ]; then
  n=$(arm-none-eabi-nm "$ELF" 2>/dev/null | grep -c -E 's_itemdesc|s_mvdesc|s_abilitydesc' || true)
  if [ "$n" -gt 0 ]; then
    echo "*** FATAL: $ELF still links s_itemdesc/s_mvdesc/s_abilitydesc ($n symbol(s))"
    fail=1
  fi
else
  echo "*** check_no_desc_text.sh: $ELF not found -- skipping the nm check" >&2
fi

if [ "$fail" -ne 0 ]; then
  echo "*** An artless build must not carry the embedded description tables -- see"
  echo "***   source/desc_gate.h, source/data_desc_shim.c, and"
  echo "***   docs/AUDIT-2026-09-05-backlog-3-19.md section B."
  exit 1
fi

echo "check_no_desc_text: OK -- no Game Freak description text in $BASE.gba/.elf"
