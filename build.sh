#!/usr/bin/env bash
#
# Build record_mixer.gba using the official devkitPro devkitARM Docker image.
# Requires only Docker. The image already contains devkitARM + libtonc.
#
set -euo pipefail
IMG="devkitpro/devkitarm:20241104"

echo ">> Building with $IMG ..."
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)":/project \
  "$IMG" \
  bash -c 'cd /project && make rebuild'

echo ">> Done. Output: $(pwd)/record_mixer.gba"
