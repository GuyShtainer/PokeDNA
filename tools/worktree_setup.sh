#!/bin/bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <dest-worktree> [--force]" >&2
  exit 1
fi

DEST="$1"
FORCE=false
if [[ $# -gt 1 ]] && [[ "$2" == "--force" ]]; then
  FORCE=true
fi

# Check that dest is a valid git worktree with the same common dir
if ! git -C "$DEST" rev-parse --git-common-dir > /dev/null 2>&1; then
  echo "Error: $DEST is not a valid git worktree" >&2
  exit 1
fi

DEST_COMMON=$(git -C "$DEST" rev-parse --git-common-dir)
MAIN_COMMON=$(git rev-parse --path-format=absolute --git-common-dir)   # absolute: a worktree's .git file stores an absolute gitdir (tiny1 review A1)

if [[ "$DEST_COMMON" != "$MAIN_COMMON" ]]; then
  echo "Error: $DEST uses a different git repository" >&2
  echo "  Destination common dir: $DEST_COMMON" >&2
  echo "  Main checkout common dir: $MAIN_COMMON" >&2
  exit 1
fi

# The main checkout is the current working directory (where the script is called from)
MAIN_REPO=$(git rev-parse --show-toplevel)

# Copy gitignored generated sources and fixtures
COPIED=0
git ls-files -o -i --exclude-standard source/ tests/fixtures/ | while read -r FILE; do
  DEST_FILE="$DEST/$FILE"

  # Skip if file exists and --force not given
  if [[ -f "$DEST_FILE" ]] && [[ "$FORCE" == false ]]; then
    continue
  fi

  # Create directory
  mkdir -p "$(dirname "$DEST_FILE")"

  # Copy with preservation of permissions/timestamps
  cp -p "$FILE" "$DEST_FILE"

  echo "copied: $FILE"
done

# Copy the generator's .sym and .asm input directories (never symlink — copy via cp -R)
# Only symbols/ and constants/ directories; data/ is not needed by gen_gbfields.py
for game in pokered pokeyellow pokegold pokecrystal; do
  if [[ -d "$MAIN_REPO/assets/upstream/$game/symbols" ]]; then
    mkdir -p "$DEST/assets/upstream/$game"
    cp -R "$MAIN_REPO/assets/upstream/$game/symbols" "$DEST/assets/upstream/$game/"
    cp -R "$MAIN_REPO/assets/upstream/$game/constants" "$DEST/assets/upstream/$game/" || true
  fi
done

# Verify all .sym files exist
for sym in assets/upstream/pokered/symbols/pokered.sym \
           assets/upstream/pokeyellow/symbols/pokeyellow.sym \
           assets/upstream/pokegold/symbols/pokegold.sym \
           assets/upstream/pokecrystal/symbols/pokecrystal.sym; do
  if [[ ! -f "$DEST/$sym" ]]; then
    echo "worktree_setup: MISSING $sym" >&2
    exit 1
  fi
done

# Run ensure_gbfields.sh to generate fresh tables in the worktree
cd "$DEST"
./tools/ensure_gbfields.sh

# Print total count
TOTAL=$(git ls-files -o -i --exclude-standard source/ tests/fixtures/ | wc -l)
echo "Total: $TOTAL files"
