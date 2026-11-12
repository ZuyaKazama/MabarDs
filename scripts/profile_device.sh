#!/usr/bin/env bash
# Profiles the running EmuNDS app on a USB/wireless-adb connected phone with simpleperf.
# Usage: start a game in the app first (with the renderer/scale you want to measure), then
#   scripts/profile_device.sh [seconds]
# Output: perf.data + report.txt (top functions of the emulation thread) in ./profile-out/
# The release APK is <profileable android:shell="true">, so no debuggable build is needed.
set -euo pipefail

SECS="${1:-15}"
PKG=com.emunds.nds
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$HOME"/Android/Sdk/ndk/* | sort -V | tail -1)}"
SP="$NDK/simpleperf"
OUT="$ROOT/profile-out"
mkdir -p "$OUT"

adb get-state >/dev/null
PID="$(adb shell pidof "$PKG" | tr -d '\r')"
[ -n "$PID" ] || { echo "EmuNDS is not running on the device"; exit 1; }

echo "== last perf lines from logcat"
adb logcat -d -s EmuNDS:I | grep "perf:" | tail -3 || true

echo "== recording $SECS s (pid $PID)"
adb push "$SP/bin/android/arm64/simpleperf" /data/local/tmp/simpleperf >/dev/null
adb shell chmod 755 /data/local/tmp/simpleperf
adb shell /data/local/tmp/simpleperf record -p "$PID" --call-graph fp -f 4000 \
    --duration "$SECS" -o /data/local/tmp/perf.data
adb pull /data/local/tmp/perf.data "$OUT/perf.data" >/dev/null

# symbols come from the unstripped library of the local build
SYMDIR="$(dirname "$(find "$ROOT/app/build/intermediates" -path '*obj/arm64-v8a/libemunds.so' | head -1)")"
python3 "$SP/report.py" -i "$OUT/perf.data" --symfs "$SYMDIR" --sort comm,dso,symbol \
    --percent-limit 0.5 > "$OUT/report.txt" 2>/dev/null || \
  "$SP/bin/linux/x86_64/simpleperf" report -i "$OUT/perf.data" --sort comm,dso,symbol \
    --percent-limit 0.5 > "$OUT/report.txt"
head -60 "$OUT/report.txt"
echo "== full report: $OUT/report.txt"
