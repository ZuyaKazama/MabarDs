#!/usr/bin/env bash
# Builds the Linux (x86-64) desktop version used for cross-play with the Android app.
#
# It is the melonDS Qt/SDL frontend compiled from the SAME pinned melonDS commit as the
# Android core (scripts/fetch_deps.sh), so the LAN multiplayer protocol is identical.
# The EmuNDS GLES patch is #ifdef MELONDS_GLES and has no effect on this desktop build.
#
# Fedora/Nobara build deps:
#   sudo dnf install cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtmultimedia-devel \
#     qt6-qtsvg-devel sdl2-compat-devel libarchive-devel libzstd-devel faad2-devel enet-devel \
#     extra-cmake-modules wayland-devel alsa-lib-devel
# Debian/Ubuntu equivalents: qt6-base-dev qt6-multimedia-dev libqt6svg6-dev libqt6opengl6-dev
#   libsdl2-dev libarchive-dev libzstd-dev libfaad-dev libenet-dev extra-cmake-modules
#   libwayland-dev libasound2-dev
#
# Output: build-linux/EmuNDS-linux/  (run ./emunds-linux.sh)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash "$ROOT/scripts/fetch_deps.sh"

SRC="$ROOT/app/src/main/cpp/external/melonDS"
BUILD="$ROOT/build-linux/cmake"
OUT="$ROOT/build-linux/EmuNDS-linux"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_QT_SDL=ON \
    -DUSE_QT6=ON \
    -DENABLE_JIT=ON \
    -DENABLE_OGLRENDERER=ON \
    -DENABLE_GDBSTUB=OFF \
    -DENABLE_LTO_RELEASE=ON \
    -DPORTABLE=ON \
    -DCMAKE_SKIP_RPATH=ON
cmake --build "$BUILD" -j"$(nproc)"

# never wipe $OUT: portable/ holds the user's config, saves and savestates
mkdir -p "$OUT"
cp "$BUILD/melonDS" "$OUT/melonDS"
cp "$ROOT/linux/emunds-linux.sh" "$ROOT/linux/README-linux.txt" "$OUT/"
chmod +x "$OUT/emunds-linux.sh" "$OUT/melonDS"
# PORTABLE build: config, saves and firmware live next to the binary
mkdir -p "$OUT/portable"
cp "$ROOT/LICENSE" "$OUT/LICENSE"

echo "done: $OUT"
