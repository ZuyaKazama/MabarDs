#!/usr/bin/env bash
# Fetches the pinned third-party sources into app/src/main/cpp/external.
set -euo pipefail

MELONDS_REPO="https://github.com/melonDS-emu/melonDS.git"
MELONDS_COMMIT="497aa6cdd26f45f8064b78916b00a773eab4ded7"
ENET_REPO="https://github.com/lsalzman/enet.git"
ENET_TAG="v1.3.18"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/app/src/main/cpp/external"
mkdir -p "$EXT"

if [ ! -f "$EXT/melonDS/src/NDS.cpp" ]; then
  rm -rf "$EXT/melonDS"
  git init -q "$EXT/melonDS"
  git -C "$EXT/melonDS" remote add origin "$MELONDS_REPO"
  git -C "$EXT/melonDS" fetch -q --depth 1 origin "$MELONDS_COMMIT"
  git -C "$EXT/melonDS" checkout -q FETCH_HEAD
fi

# EmuNDS patches (GLES 3.2 port of the OpenGL renderer); idempotent
for p in "$ROOT"/patches/*.patch; do
  [ -f "$p" ] || continue
  if git -C "$EXT/melonDS" apply --check "$p" 2>/dev/null; then
    git -C "$EXT/melonDS" apply "$p"
    echo "applied $(basename "$p")"
  fi
done

if [ ! -f "$EXT/enet/include/enet/enet.h" ]; then
  rm -rf "$EXT/enet"
  git clone -q --depth 1 --branch "$ENET_TAG" "$ENET_REPO" "$EXT/enet"
fi

echo "deps ready in $EXT"
